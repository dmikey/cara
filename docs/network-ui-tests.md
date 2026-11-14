# Room screen UI regression fixture

`tools/build_ios_network_preview.py` builds a small simulator app containing the real `CaraNetworkController` and a fake status provider. It links only UIKit and Foundation: no emulator engine, relay client, sockets, game data or device access. The screen and receipts explicitly label the connection as a UI fake.

The fixture checks the callback contract with assertions: trimmed room name, unchanged password, password field cleared before the join callback, no duplicate join while active, and no disconnect while inactive. It uses a bounded five-second fake connection completion with a generation token to avoid completing a canceled attempt. It records callback counts without saving or printing passwords.

The separate `CaraNetworkUISmoke` project exercises the actual simulator UI: server/room/password fields; secure password entry; clear-on-join and clear-on-navigation; Connecting/Connected/Disconnected labels; enabled/disabled actions; and rejected credential/query/fragment endpoints, including empty-present URL components. These tests establish UI behavior only. They do not establish real relay connectivity, packet delivery, UDS admission or multiplayer gameplay.

Example commands, using a dedicated simulator selected by the developer:

```sh
python3 tools/build_ios_network_preview.py --output work/network-ui
xcrun simctl install "$CARA_UI_SIM" work/network-ui/CaraNetworkPreview.app
xcodebuild test -project tools/ios_network_ui_tests/CaraNetworkUISmoke.xcodeproj \
  -scheme CaraNetworkUISmoke -destination "platform=iOS Simulator,id=$CARA_UI_SIM" \
  -derivedDataPath work/network-ui/derived -resultBundlePath work/network-ui/results.xcresult \
  -parallel-testing-enabled NO CODE_SIGNING_ALLOWED=NO
```

The production controller matches the transport's URL policy (`wss`, nonempty host, exact `/relay`, no credential/query/fragment components) and UTF-8 length limits (room1–64 bytes, password1–512 bytes). It checks active state again when handling an action, and invalidates its previous refresh timer before starting another. Passwords are not stored in defaults or echoed in validation messages.

## Local simulator evidence

On 2026-11-13 the dedicated iPhone simulator passed all three cases: join/connection states/password clear/action availability (21.055s); password clear when navigating away and cancellation of the pending fake connection (27.617s); and five exact invalid URL forms rejected without invoking join (42.766s). The first two passed in the combined run after the controller's stable-update fix; URL validation passed in its final focused run after correcting the test helper's handling of a real default value equal to its placeholder. Earlier failed fixture/tool runs are not success evidence.

The controller now leaves unchanged status/action rows intact between timer ticks, preserving taps and accessibility focus. Join and Disconnect are explicitly labeled accessibility elements. The UI tests query their identifiers independent of element type because iOS27 reports UITableViewCell button traits with a legacy Cell automation type.
