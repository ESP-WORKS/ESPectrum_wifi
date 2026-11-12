# Flash only firmware.bin into the multiloader ota_0 slot (0x130000).
# ESP-IDF's default upload writes the app to factory (0x10000 / 0x120000),
# which is too small and overwrites the loader.
Import("env")

OTA0_OFFSET = "0x130000"

env.Replace(
    UPLOADCMD=" ".join(
        [
            '"$PYTHONEXE"',
            '"$UPLOADER"',
            "--chip",
            "esp32",
            "--port",
            '"$UPLOAD_PORT"',
            "--baud",
            "$UPLOAD_SPEED",
            "--before",
            "default_reset",
            "--after",
            "hard_reset",
            "write_flash",
            "-z",
            "--flash_size",
            "4MB",
            OTA0_OFFSET,
            '"$BUILD_DIR/firmware.bin"',
        ]
    )
)

print("ESPectrum: upload writes firmware.bin to ota_0 @ %s only (factory/multiloader kept)." % OTA0_OFFSET)
