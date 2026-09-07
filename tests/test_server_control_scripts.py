from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_root_server_shortcuts_delegate_to_the_combined_server():
    start = (ROOT / "start_server.bat").read_text(encoding="utf-8").lower()
    stop = (ROOT / "stop_server.bat").read_text(encoding="utf-8").lower()

    assert "tools\\youtube_tv_server\\start_server.bat" in start
    assert "tools\\youtube_tv_server\\stop_server.bat" in stop


def test_combined_server_stop_script_targets_only_minitv_server_port():
    script = (ROOT / "tools" / "youtube_tv_server" / "stop_server.bat").read_text(encoding="utf-8").lower()
    helper = (ROOT / "tools" / "youtube_tv_server" / "stop_server_background.ps1").read_text(encoding="utf-8").lower()

    assert "stop_server_background.ps1" in script
    assert "server.pid" in script
    assert "get-nettcpconnection" in helper
    assert "-localport 8876" in helper
    assert "server.py" in helper
    assert "stop-process" in helper
    assert "*$serverdir*server.py*" not in helper


def test_start_script_has_a_platformio_python_fallback():
    script = (ROOT / "tools" / "youtube_tv_server" / "start_server.bat").read_text(encoding="utf-8").lower()

    assert ".platformio\\penv\\scripts\\python.exe" in script


def test_start_script_prefers_installed_python_312_over_an_older_path_python():
    script = (ROOT / "tools" / "youtube_tv_server" / "start_server.bat").read_text(encoding="utf-8").lower()

    python312 = script.index("%localappdata%\\programs\\python\\python312\\python.exe")
    first_path_lookup = script.index("where python")
    assert python312 < first_path_lookup


def test_server_shortcuts_manage_a_background_process_with_a_pid_file():
    start = (ROOT / "tools" / "youtube_tv_server" / "start_server.bat").read_text(encoding="utf-8").lower()
    stop = (ROOT / "tools" / "youtube_tv_server" / "stop_server.bat").read_text(encoding="utf-8").lower()
    helper = (ROOT / "tools" / "youtube_tv_server" / "start_server_background.ps1").read_text(encoding="utf-8").lower()

    assert "start_server_background.ps1" in start
    assert "server.pid" in helper
    assert "start-process" in helper
    assert "-windowstyle hidden" in helper
    assert "server.pid" in stop


def test_readme_documents_root_start_and_stop_shortcuts():
    readme = (ROOT / "README.md").read_text(encoding="utf-8").lower()

    assert ".\\start_server.bat" in readme
    assert ".\\stop_server.bat" in readme


def test_install_script_installs_server_runtime_dependencies():
    script = (ROOT / "install.bat").read_text(encoding="utf-8").lower()

    assert "winget" in script
    assert "python.python.3.12" in script
    assert "gyan.ffmpeg.shared" in script
    assert "openjs.nodejs.lts" in script
    assert "google.chrome" not in script
    assert "requirements.txt" in script
    requirements = (ROOT / "tools" / "youtube_tv_server" / "requirements.txt").read_text(encoding="utf-8").lower()
    assert "playwright" not in requirements


def test_install_script_prefers_new_python_312_over_an_older_path_python():
    script = (ROOT / "install.bat").read_text(encoding="utf-8").lower()

    python312 = script.index("%localappdata%\\programs\\python\\python312\\python.exe")
    first_path_lookup = script.index("where python")
    assert python312 < first_path_lookup


def test_install_script_never_builds_native_dependencies_from_source():
    script = (ROOT / "install.bat").read_text(encoding="utf-8").lower()

    assert "--only-binary=:all:" in script


if __name__ == "__main__":
    test_root_server_shortcuts_delegate_to_the_combined_server()
    test_combined_server_stop_script_targets_only_minitv_server_port()
    test_start_script_has_a_platformio_python_fallback()
    test_start_script_prefers_installed_python_312_over_an_older_path_python()
    test_server_shortcuts_manage_a_background_process_with_a_pid_file()
    test_readme_documents_root_start_and_stop_shortcuts()
    test_install_script_installs_server_runtime_dependencies()
    test_install_script_prefers_new_python_312_over_an_older_path_python()
    test_install_script_never_builds_native_dependencies_from_source()
    print("PASS")


def test_start_helper_restarts_existing_server_to_load_current_source():
    helper = (ROOT / "tools" / "youtube_tv_server" / "start_server_background.ps1").read_text(encoding="utf-8").lower()
    # Starting the server after source changes must not silently keep the old Python process.
    assert "mini tv server dang chay nen" not in helper
    assert helper.index("stop-process") < helper.index("start-process")
