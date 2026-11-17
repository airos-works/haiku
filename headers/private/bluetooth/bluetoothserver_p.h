#ifndef _BLUETOOTH_SERVER_PRIVATE_H
#define _BLUETOOTH_SERVER_PRIVATE_H


#define BLUETOOTH_SIGNATURE "application/x-vnd.Haiku-bluetooth_server"
#define BLUETOOTH_APP_SIGNATURE "application/x-vnd.Haiku-BluetoothPrefs"

/* Kit Comunication */
// Watching
#define BT_START_WATCHING_CONNECTIONS               'wtST'
#define BT_STOP_WATCHING_CONNECTIONS                'wtSP'

// LocalDevice
#define BT_MSG_COUNT_LOCAL_DEVICES		'btCd'
#define BT_MSG_ACQUIRE_LOCAL_DEVICE     'btAd'
#define BT_MSG_HANDLE_SIMPLE_REQUEST    'btsR'
#define BT_MSG_ADD_DEVICE               'btDD'
#define BT_MSG_REMOVE_DEVICE            'btrD'
#define BT_MSG_GET_PROPERTY             'btgP'
#define BT_MSG_GET_REMOTE_DEVICES       'btgD'
#define BT_MSG_NEW_REMOTE_DEVICE        'btnD'
#define BT_MSG_PAIR_CONFIRM_RESULT       'btPC'

// LE discovery. Advertising messages carry a raw address and advertisement.
#define BT_MSG_LE_SCAN_START            'leSS'
#define BT_MSG_LE_SCAN_STOP             'leST'
#define BT_MSG_LE_SCAN_STARTED          'leSA'
#define BT_MSG_LE_SCAN_STOPPED          'leSP'
#define BT_MSG_LE_SCAN_ERROR            'leSE'
#define BT_MSG_LE_ADVERTISEMENT         'leAD'

// LE ACL link lifecycle. These messages do not imply pairing or a HID profile.
// A controller holds several links, one per peer, each owned by the listener
// that asked for it; a connect to a peer that already has a link (or a
// request for one) fails with B_BUSY. Requests queue: the controller creates
// one connection at a time. Scan stop, connect cancel and disconnect take
// the client's "listener" so one client cannot end another's scan or link,
// and may take the peer's "address" to pick one of the listener's links
// (all of them otherwise); BT_MSG_LE_DISCONNECT also accepts bool "force"
// to end the link (with "address") or the links whoever owns them. Link
// notices carry the peer's "address" and the link's "handle".
#define BT_MSG_LE_CONNECT               'leCN'
#define BT_MSG_LE_CONNECT_CANCEL        'leCC'
#define BT_MSG_LE_DISCONNECT            'leDC'
#define BT_MSG_LE_CONNECTING            'leCG'
#define BT_MSG_LE_CONNECTED             'leCD'
#define BT_MSG_LE_CONNECT_FAILED        'leCF'
#define BT_MSG_LE_DISCONNECTED          'leDD'
// Request: either short_term_key (16 bytes), or long_term_key (16 bytes),
// random_number (8 bytes) and encrypted_diversifier (uint16). This,
// BT_MSG_LE_UPDATE_CONNECTION and BT_MSG_READ_RSSI act on the link with the
// request's "handle" or "address", else the one link of its "listener",
// else the only LE link there is.
#define BT_MSG_LE_START_ENCRYPTION      'leEN'
// Asks the controller for other parameters on the LE link: "interval_min",
// "interval_max" (1.25 ms units), "latency" (events), "timeout" (10 ms),
// the last two the link's if left out; "keep_interval" (bool) asks for the
// interval again whenever the peer changes it. BT_MSG_LE_CONNECT takes the same four fields (optional) for
// the connection it creates.
#define BT_MSG_LE_UPDATE_CONNECTION     'leUP'
// Logs the signal strength of a link: "handle" (uint16, the LE link if left
// out); the answer goes to the LE log.
#define BT_MSG_READ_RSSI                'leRS'
#define BT_MSG_LE_ENCRYPTED             'leED'
#define BT_MSG_LE_ENCRYPTION_FAILED     'leEF'

// Pairing for applications: secures the LE link the requester holds. The
// request names the link with "hci_id" and the "listener" that owns it
// (plus the peer's "address" when the listener holds several); another
// client's link is refused with B_NOT_ALLOWED. When a bond for the peer is
// stored (under this address, its identity address, or resolved with its
// IRK), the link is encrypted with it and nobody is asked. Otherwise the
// server pairs: LE Secure Connections when the device supports it (legacy
// pairing else), after the user agrees in a system prompt that names the
// application and the device (Just Works), shows the six digits to compare
// (Numeric Comparison) or the passkey to type on the device (Passkey Entry).
// The new bond is stored for the next connection.
// Optional: "device_name" (string, shown in the prompt), "fresh" (bool,
// pair again even with a bond), "bonded_only" (bool, only use a stored bond,
// never prompt or pair).
// The reply (BT_MSG_LE_PAIR_RESULT) comes when it is done, which may take a
// few minutes with the prompt; send the request with a reply handler or a
// long reply timeout. Its "status" (int32) is B_OK, B_CANCELED (the user
// declined or did not answer), B_PERMISSION_DENIED (the device refused),
// B_BAD_DATA (the device's values did not check out), B_ENTRY_NOT_FOUND
// (bonded_only without a bond), B_BUSY (another pairing runs on the link),
// B_NOT_ALLOWED, B_TIMED_OUT, or an error. Also: "smp_reason" (uint8, the
// SMP Pairing Failed reason, 0 for none), "smp_reason_remote" (bool, the
// device sent it), "hci_status" (uint8, of a failed encryption),
// "encrypted", "bonded", "reused_bond", "secure_connections",
// "authenticated" (bools), "method" (int32, LESMPMethod, -1 for none),
// "key_size" (uint8) and "detail" (string, a sentence for the user).
// The link's listener also gets BT_MSG_LE_ENCRYPTED or
// BT_MSG_LE_ENCRYPTION_FAILED as usual.
#define BT_MSG_LE_PAIR                  'lePR'
#define BT_MSG_LE_PAIR_RESULT           'lePD'

// Discovery
#define BT_MSG_INQUIRY_STARTED          'IqSt'
#define BT_MSG_INQUIRY_COMPLETED        'IqCM'
#define BT_MSG_INQUIRY_TERMINATED       'IqTR'
#define BT_MSG_INQUIRY_ERROR            'IqER'
#define BT_MSG_INQUIRY_DEVICE           'IqDE'

// Pairing
#define BT_MSG_CONN_FAILED              'CnFL'
#define BT_MSG_CONN_COMPLETED           'CnCM'
#define BT_MSG_DISCONN_COMPLETED        'DcCM'

#define BT_REQ_CREATE_CONN              'rdCN'
#define BT_REQ_CANCEL_CONN              'rdCC'
#define BT_REQ_DISCONNECT               'rdDC'
#define BT_REQ_REMOVE_DEVICE            'rdRD'

#define BT_REQ_CONN_STATE               'rdCS'


#endif
