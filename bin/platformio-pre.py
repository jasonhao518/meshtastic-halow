#!/usr/bin/env python3
# trunk-ignore-all(ruff/F821)
# trunk-ignore-all(flake8/F821): For SConstruct imports
Import("env")
platform = env.PioPlatform()

if platform.name == "native":
    env.Replace(PROGNAME="meshtasticd")
else:
    from readprops import readProps
    prefsLoc = env["PROJECT_DIR"] + "/version.properties"
    verObj = readProps(prefsLoc)
    env.Replace(PROGNAME=f"firmware-{env.get('PIOENV')}-{verObj['long']}")
    env.Replace(ESP32_FS_IMAGE_NAME=f"littlefs-{env.get('PIOENV')}-{verObj['long']}")

# Print the new program name for verification
print(f"PROGNAME: {env.get('PROGNAME')}")
if platform.name == "espressif32":
    print(f"ESP32_FS_IMAGE_NAME: {env.get('ESP32_FS_IMAGE_NAME')}")

# Consume the custom metadata as a compile-time hardware model selector.
hw_model = env.GetProjectOption("custom_meshtastic_hw_model", None)
if hw_model:
    try:
        env.Append(CPPDEFINES=[("MESHTASTIC_HW_MODEL", int(hw_model))])
    except Exception as ex:
        print(f"Invalid custom_meshtastic_hw_model={hw_model!r}: {ex}")
