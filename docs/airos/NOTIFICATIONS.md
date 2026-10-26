# Notifications that answer

air/OS's notification_server can tell the application that sent a notification
what became of it, instead of launching an application when it is clicked
(`BNotification::SetOnClickApp()` and friends, which still work). There are no
`BNotification` calls for this: archive the notification and add the fields
below to the archive before sending it to the server.

| Field | Type | Meaning |
| --- | --- | --- |
| `_reply:target` | `BMessenger` | Where the answers go. Without it, nothing is sent. |
| `_reply:activated` | `BMessage` | Sent when the notification is clicked (`"button"` 0) or one of its buttons is (`"button"` 1, 2, …). Its presence replaces launching the on-click application. |
| `_reply:closed` | `BMessage` | Sent when the notification goes away, unless a notification with the same message ID replaced it. `"by_user"` (bool) is true when the user clicked it, one of its buttons, its close button or its group's close button, false when it timed out. |
| `_buttons` | strings | Labels of buttons shown under the text, numbered from 1 in that order. |

The answers are copies of the messages given, with `"button"` (activated) or
`"by_user"` (closed) set; put whatever identifies the notification into them.

```cpp
BNotification notification(B_INFORMATION_NOTIFICATION);
notification.SetTitle("Download finished");
notification.SetContent("report.pdf");
notification.SetMessageID("download-42");

BMessage request(kNotificationMessage);	// as BNotification::Send() builds it
notification.Archive(&request);
request.AddMessenger("_reply:target", BMessenger(this));

BMessage activated(kMsgNotificationActivated);
activated.AddString("id", "download-42");
request.AddMessage("_reply:activated", &activated);

BMessage closed(kMsgNotificationClosed);
closed.AddString("id", "download-42");
request.AddMessage("_reply:closed", &closed);

request.AddString("_buttons", "Open");
request.AddString("_buttons", "Show in folder");

BMessenger(kNotificationServerSignature).SendMessage(&request);
```

`kNotificationMessage` is the notification_server's request code (`'nssm'`,
`headers/private/notification/Notifications.h`). Terminal uses all of this for
kitty's OSC 99 notifications (`src/apps/terminal/TermView.cpp`).

A notification_server without this support ignores the fields and launches the
on-click application as before, so set both if the application has to work
there too.
