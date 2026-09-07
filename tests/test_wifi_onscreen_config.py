from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
PIO = (ROOT / "platformio.ini").read_text(encoding="utf-8")


def test_wifi_configuration_never_starts_a_config_ap():
    assert "#include <WiFiManager.h>" not in MAIN
    assert "WiFiManager" not in MAIN
    assert "autoConnect(" not in MAIN
    assert "startConfigPortal(" not in MAIN
    assert "softAP(" not in MAIN
    assert "WiFiManager@" not in PIO


def test_wifi_has_touchscreen_ssid_list_and_password_keyboard():
    assert "WIFI_LIST" in MAIN
    assert "WIFI_PASSWORD" in MAIN
    assert "scanWiFiForUi" in MAIN
    assert "drawWiFiList" in MAIN
    assert "handleWiFiListTouch" in MAIN
    assert "drawWiFiPassword" in MAIN
    assert "handleWiFiPasswordTouch" in MAIN
    assert "WiFi.scanNetworks" in MAIN
    assert "wifiSelectedSsid" in MAIN
    assert "wifiPassword" in MAIN
    assert "WIFI_KB_DIGITS" in MAIN
    assert "WIFI_KB_SYMBOLS" in MAIN


def test_wifi_credentials_are_saved_locally_and_reused_on_boot():
    assert 'prefs.begin("cydwifi"' in MAIN
    assert 'prefs.putString("ssid"' in MAIN
    assert 'prefs.putString("pass"' in MAIN
    assert 'prefs.getString("ssid"' in MAIN
    assert 'prefs.getString("pass"' in MAIN
    assert "connectSavedWiFi" in MAIN
    assert "WiFi.begin(ssid.c_str(), password.c_str())" in MAIN
    setup = MAIN.split("void setup() {", 1)[1].split("void loop() {", 1)[0]
    assert "connectSavedWiFi" in setup
    assert "Screen::WIFI_LIST" in setup


def test_home_header_wifi_button_opens_manual_wifi_setup_without_footer_duplicate():
    home_draw = MAIN.split("static void drawHome()", 1)[1].split("static void handleHomeTouch()", 1)[0]
    home_touch = MAIN.split("static void handleHomeTouch()", 1)[1].split("void setup()", 1)[0]
    assert 'fillRounded(4, 3, 64, 22' in home_draw
    assert 'ui().text("WiFi", 36, 14, 1)' in home_draw
    assert 'WiFi.status() == WL_CONNECTED ? TFT_GREEN : TFT_LIGHTGREY' in home_draw
    assert 'y < 32 && x < 80' in home_touch
    assert 'enterWiFiSetup(Screen::HOME)' in home_touch
    assert 'y >= 226' not in home_touch
    assert 'WiFi - chạm để cấu hình' not in home_draw
    assert 'WiFi - tap to configure' not in home_draw


def test_failed_network_use_redirects_to_onscreen_setup_not_portal():
    ensure = MAIN.split("static bool ensureWiFi()", 1)[1].split("static IPAddress subnetBroadcast", 1)[0]
    assert "connectSavedWiFi" in ensure
    assert "enterWiFiSetup" in ensure
    assert "WiFiManager" not in ensure


if __name__ == "__main__":
    tests = [v for k, v in list(globals().items()) if k.startswith("test_") and callable(v)]
    for test in tests:
        test()
        print("PASS", test.__name__)
