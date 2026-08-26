Import("env")
from os.path import join

platform = env.PioPlatform()
project_dir = env.subst("$PROJECT_DIR")
esptool = join(platform.get_package_dir("tool-esptoolpy"), "esptool.py")
framework = platform.get_package_dir("framework-arduinoespressif32")
wrapper = join(project_dir, "scripts", "upload_cyd_retry.py")

env.Replace(
    UPLOADCMD='"$PYTHONEXE" "{}" --port "$UPLOAD_PORT" --baud "$UPLOAD_SPEED" --esptool "{}" --framework "{}" --build-dir "$BUILD_DIR"'.format(
        wrapper, esptool, framework
    )
)
