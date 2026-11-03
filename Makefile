.PHONY: server test firmware flash

server:
	cd server && ../.venv/bin/uvicorn app.main:app --host 0.0.0.0 --port 8000

test:
	cd server && ../.venv/bin/python -m pytest -q

firmware:
	. $(HOME)/esp-idf/export.sh && cd firmware && idf.py build && idf.py merge-bin -o build/merged.bin

# The ESP32-S3 is on Windows COM4; flashing goes through the Windows esptool.
flash:
	/mnt/c/Windows/System32/cmd.exe /c "py -3 -m esptool --chip esp32s3 -p COM4 -b 921600 write_flash 0x0 \\\\wsl.localhost\\Ubuntu\\home\\luispoveda93\\plaud-s3\\firmware\\build\\merged.bin"
