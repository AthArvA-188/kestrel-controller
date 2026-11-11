// Bluetooth LE HID over GATT with NimBLE. Xbox-identity mode presents the
// Xbox Wireless Controller's report map and PnP ID. Generic mode presents the
// same HID gamepad as USB DInput. Neither has been tried against a host.
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "ks_reports.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

void ble_store_config_init(void); // NimBLE's NVS bond store has no public header

static const char *TAG = "ble";

#define APPEARANCE_GAMEPAD 0x03C4
#define UUID_HID_INFO 0x2A4A
#define UUID_REPORT_MAP 0x2A4B
#define UUID_HID_CONTROL 0x2A4C
#define UUID_REPORT 0x2A4D
#define UUID_PROTOCOL_MODE 0x2A4E
#define UUID_REPORT_REF 0x2908

enum { CHR_MANUFACTURER, CHR_MODEL, CHR_FIRMWARE, CHR_PNP };
enum { HID_INFO, HID_MAP, HID_CONTROL, HID_PROTOCOL, HID_INPUT, HID_OUT_RUMBLE, HID_OUT_FX };

static ks_mode_t ble_mode;
static uint8_t own_addr_type;
static uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t input_handle, battery_handle;
static bool notify_on;
static uint8_t battery_pct = 100;
static uint8_t last_report[KS_XBOX_BLE_INPUT_LEN];
static char peer[24];

// Report reference values: report ID, then 1 for input or 2 for output.
static const uint8_t ref_in[2] = {KS_HID_ID_INPUT, 1};
static const uint8_t ref_out_rumble[2] = {KS_HID_ID_RUMBLE, 2};
static const uint8_t ref_out_fx[2] = {KS_HID_ID_TRIGGER_FX, 2};
static const uint8_t ref_xbox_in[2] = {KS_XBOX_BLE_ID_INPUT, 1};
static const uint8_t ref_xbox_out[2] = {KS_XBOX_BLE_ID_RUMBLE, 2};

static bool xbox(void) { return ble_mode == KS_MODE_BLE_XBOX; }

static int append(struct os_mbuf *om, const void *data, size_t len)
{
    return os_mbuf_append(om, data, (uint16_t)len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int dis_access(uint16_t conn_handle, uint16_t attr, struct ble_gatt_access_ctxt *ctxt,
                      void *arg)
{
    // PnP ID: vendor ID source (2 is USB-IF), vendor, product, version.
    static const uint8_t pnp_xbox[7] = {0x02, KS_XBOX_BLE_VID & 0xFF, KS_XBOX_BLE_VID >> 8,
                                        KS_XBOX_BLE_PID & 0xFF, KS_XBOX_BLE_PID >> 8, 0x09, 0x05};
    static const uint8_t pnp_generic[7] = {0x02, KS_USB_VID & 0xFF, KS_USB_VID >> 8,
                                           KS_USB_PID & 0xFF, KS_USB_PID >> 8,
                                           KS_FW_VERSION & 0xFF, KS_FW_VERSION >> 8};
    const char *text;
    switch ((intptr_t)arg) {
    case CHR_MANUFACTURER: text = xbox() ? "Microsoft" : "Kestrel"; break;
    case CHR_MODEL: text = xbox() ? "Xbox Wireless Controller" : "Kestrel Controller"; break;
    case CHR_FIRMWARE: text = "0.1.0"; break;
    default: return append(ctxt->om, xbox() ? pnp_xbox : pnp_generic, 7);
    }
    return append(ctxt->om, text, strlen(text));
}

static int battery_access(uint16_t conn_handle, uint16_t attr, struct ble_gatt_access_ctxt *ctxt,
                          void *arg)
{
    return append(ctxt->om, &battery_pct, 1);
}

static int ref_access(uint16_t conn_handle, uint16_t attr, struct ble_gatt_access_ctxt *ctxt,
                      void *arg)
{
    return append(ctxt->om, arg, 2);
}

static void handle_output(int kind, const uint8_t *buf, size_t len)
{
    ks_rumble_t rumble;
    ks_trigger_fx_cfg_t fx;
    uint8_t target;
    if (kind == HID_OUT_RUMBLE) {
        if (xbox() ? ks_xbox_ble_parse_rumble(buf, len, &rumble)
                   : ks_hid_parse_rumble(buf, len, &rumble)) {
            app_set_rumble(rumble);
        }
    } else if (ks_hid_parse_trigger_fx(buf, len, &target, &fx)) {
        app_set_trigger_fx(target, &fx);
    }
}

static int hid_access(uint16_t conn_handle, uint16_t attr, struct ble_gatt_access_ctxt *ctxt,
                      void *arg)
{
    // bcdHID 1.11, no country code, remote wake and normally connectable.
    static const uint8_t info[4] = {0x11, 0x01, 0x00, 0x03};
    static const uint8_t report_protocol = 1;
    int kind = (int)(intptr_t)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t buf[16];
        uint16_t len = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof buf, &len) != 0) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        if (kind == HID_OUT_RUMBLE || kind == HID_OUT_FX) {
            handle_output(kind, buf, len);
        }
        return 0; // control point and protocol mode writes need no action
    }
    switch (kind) {
    case HID_INFO: return append(ctxt->om, info, sizeof info);
    case HID_MAP:
        return xbox() ? append(ctxt->om, ks_hid_desc_xbox_ble, ks_hid_desc_xbox_ble_len)
                      : append(ctxt->om, ks_hid_desc_gamepad, ks_hid_desc_gamepad_len);
    case HID_PROTOCOL: return append(ctxt->om, &report_protocol, 1);
    case HID_INPUT:
        return append(ctxt->om, last_report, xbox() ? KS_XBOX_BLE_INPUT_LEN : KS_HID_INPUT_LEN);
    default: return 0;
    }
}

#define CHR_READ(uuid16, cb, id) \
    {.uuid = BLE_UUID16_DECLARE(uuid16), .access_cb = (cb), .arg = (void *)(intptr_t)(id), \
     .flags = BLE_GATT_CHR_F_READ}

// A Report characteristic with its Report Reference descriptor. Reports need
// an encrypted link, which is what makes a host pair on first use.
#define CHR_REPORT(kind, ref, extra, handle) \
    {.uuid = BLE_UUID16_DECLARE(UUID_REPORT), .access_cb = hid_access, \
     .arg = (void *)(intptr_t)(kind), .val_handle = (handle), \
     .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | (extra), \
     .descriptors = (struct ble_gatt_dsc_def[]){ \
         {.uuid = BLE_UUID16_DECLARE(UUID_REPORT_REF), .att_flags = BLE_ATT_F_READ, \
          .access_cb = ref_access, .arg = (void *)(ref)}, \
         {0}}}

#define CHR_INPUT(ref) CHR_REPORT(HID_INPUT, ref, BLE_GATT_CHR_F_NOTIFY, &input_handle)
#define CHR_OUTPUT(kind, ref) \
    CHR_REPORT(kind, ref, BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | \
                              BLE_GATT_CHR_F_WRITE_ENC, NULL)

#define HID_COMMON_CHRS \
    CHR_READ(UUID_HID_INFO, hid_access, HID_INFO), CHR_READ(UUID_REPORT_MAP, hid_access, HID_MAP), \
    {.uuid = BLE_UUID16_DECLARE(UUID_HID_CONTROL), .access_cb = hid_access, \
     .arg = (void *)(intptr_t)HID_CONTROL, .flags = BLE_GATT_CHR_F_WRITE_NO_RSP}, \
    {.uuid = BLE_UUID16_DECLARE(UUID_PROTOCOL_MODE), .access_cb = hid_access, \
     .arg = (void *)(intptr_t)HID_PROTOCOL, \
     .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP}

static const struct ble_gatt_chr_def dis_chrs[] = {
    CHR_READ(0x2A29, dis_access, CHR_MANUFACTURER), CHR_READ(0x2A24, dis_access, CHR_MODEL),
    CHR_READ(0x2A26, dis_access, CHR_FIRMWARE),     CHR_READ(0x2A50, dis_access, CHR_PNP),
    {0},
};

static const struct ble_gatt_chr_def battery_chrs[] = {
    {.uuid = BLE_UUID16_DECLARE(0x2A19), .access_cb = battery_access,
     .val_handle = &battery_handle, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY},
    {0},
};

static const struct ble_gatt_chr_def hid_chrs_xbox[] = {
    HID_COMMON_CHRS,
    CHR_INPUT(ref_xbox_in),
    CHR_OUTPUT(HID_OUT_RUMBLE, ref_xbox_out),
    {0},
};

static const struct ble_gatt_chr_def hid_chrs_generic[] = {
    HID_COMMON_CHRS,
    CHR_INPUT(ref_in),
    CHR_OUTPUT(HID_OUT_RUMBLE, ref_out_rumble),
    CHR_OUTPUT(HID_OUT_FX, ref_out_fx),
    {0},
};

#define SERVICES(hid_chrs) \
    {{.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = BLE_UUID16_DECLARE(0x180A), \
      .characteristics = dis_chrs}, \
     {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = BLE_UUID16_DECLARE(0x180F), \
      .characteristics = battery_chrs}, \
     {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = BLE_UUID16_DECLARE(0x1812), \
      .characteristics = (hid_chrs)}, \
     {0}}

static const struct ble_gatt_svc_def services_xbox[] = SERVICES(hid_chrs_xbox);
static const struct ble_gatt_svc_def services_generic[] = SERVICES(hid_chrs_generic);

static const char *device_name(void) { return xbox() ? "Xbox Wireless Controller" : "Kestrel"; }

static void advertise(void);

static int gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            advertise();
            break;
        }
        conn = event->connect.conn_handle;
        if (ble_gap_conn_find(conn, &desc) == 0) {
            const uint8_t *a = desc.peer_id_addr.val;
            snprintf(peer, sizeof peer, "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2],
                     a[1], a[0]);
        }
        // Ask for a short connection interval, 7.5 to 15 ms, and start
        // pairing without waiting for the host to hit an encrypted report.
        struct ble_gap_upd_params params = {
            .itvl_min = 6, .itvl_max = 12, .latency = 0, .supervision_timeout = 300,
        };
        ble_gap_update_params(conn, &params);
        ble_gap_security_initiate(conn);
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        conn = BLE_HS_CONN_HANDLE_NONE;
        notify_on = false;
        peer[0] = '\0';
        app_clear_host_state();
        advertise();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == input_handle) {
            notify_on = event->subscribe.cur_notify;
        }
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        // The host lost its bond. Drop ours and let it pair again.
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    default:
        break;
    }
    return 0;
}

static void advertise(void)
{
    const char *name = device_name();
    struct ble_hs_adv_fields adv = {0}, rsp = {0};
    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = 0x30, // 30 ms
        .itvl_max = 0x60, // 60 ms
    };
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.appearance = APPEARANCE_GAMEPAD;
    adv.appearance_is_present = 1;
    adv.uuids16 = (ble_uuid16_t[]){BLE_UUID16_INIT(0x1812)};
    adv.num_uuids16 = 1;
    adv.uuids16_is_complete = 1;
    // The Xbox name does not fit beside the other fields, so it goes in the
    // scan response.
    rsp.name = (const uint8_t *)name;
    rsp.name_len = (uint8_t)strlen(name);
    rsp.name_is_complete = 1;
    if (ble_gap_adv_set_fields(&adv) != 0 || ble_gap_adv_rsp_set_fields(&rsp) != 0 ||
        ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL) != 0) {
        ESP_LOGE(TAG, "advertising did not start");
    }
}

static void on_sync(void)
{
    if (xbox()) {
        // A static random address keeps the two identities apart. A host that
        // paired with one mode would otherwise reuse its cached services for
        // the other.
        uint8_t mac[6], addr[6];
        esp_read_mac(mac, ESP_MAC_BT);
        for (int i = 0; i < 6; i++) {
            addr[i] = mac[5 - i];
        }
        addr[0] ^= 0x5A;
        addr[5] |= 0xC0;
        ble_hs_id_set_rnd(addr);
        own_addr_type = BLE_OWN_ADDR_RANDOM;
    } else {
        ble_hs_util_ensure_addr(0);
        ble_hs_id_infer_auto(0, &own_addr_type);
    }
    advertise();
}

static void on_reset(int reason) { ESP_LOGW(TAG, "host reset, reason %d", reason); }

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_hid_start(ks_mode_t mode)
{
    ble_mode = mode;
    const struct ble_gatt_svc_def *services = xbox() ? services_xbox : services_generic;

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    // Bonding with LE Secure Connections. The pad has no keys or display for
    // a passkey, so pairing is Just Works.
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_ERROR_CHECK(ble_gatts_count_cfg(services) == 0 ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(ble_gatts_add_svcs(services) == 0 ? ESP_OK : ESP_FAIL);
    ble_svc_gap_device_name_set(device_name());
    ble_svc_gap_device_appearance_set(APPEARANCE_GAMEPAD);
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "started as %s", ks_mode_name(mode));
}

// Notifies the input report when it changed. The caller limits the rate.
void ble_hid_send(const ks_gamepad_t *pad)
{
    uint8_t report[KS_XBOX_BLE_INPUT_LEN];
    size_t len = xbox() ? KS_XBOX_BLE_INPUT_LEN : KS_HID_INPUT_LEN;
    if (xbox()) {
        ks_xbox_ble_build(pad, report);
    } else {
        ks_hid_build(pad, report);
    }
    if (memcmp(report, last_report, len) == 0) {
        return;
    }
    memcpy(last_report, report, len);
    if (conn != BLE_HS_CONN_HANDLE_NONE && notify_on) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(report, (uint16_t)len);
        if (om) {
            ble_gatts_notify_custom(conn, input_handle, om);
        }
    }
}

void ble_hid_set_battery(uint8_t pct)
{
    if (pct != battery_pct) {
        battery_pct = pct;
        if (battery_handle) {
            ble_gatts_chr_updated(battery_handle);
        }
    }
}

void ble_hid_status(link_status_t *st)
{
    bool up = conn != BLE_HS_CONN_HANDLE_NONE;
    st->link = up ? KS_LINK_CONNECTED : KS_LINK_SEARCHING;
    snprintf(st->host, sizeof st->host, "%s", up ? peer : "");
    if (up) {
        snprintf(st->detail, sizeof st->detail, "%s",
                 xbox() ? "Xbox Wireless Controller identity" : "HID gamepad");
    } else {
        snprintf(st->detail, sizeof st->detail, "Pair with \"%s\"", device_name());
    }
}
