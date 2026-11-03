# Override on the command line: make flash PORT=/dev/ttyACM0
PORT ?= /dev/ttyACM0
IDF_EXPORT ?= $(HOME)/esp-idf/export.sh
VENV ?= .venv

.PHONY: server test lint coverage firmware flash

server:
	cd server && ../$(VENV)/bin/uvicorn app.main:app --host 0.0.0.0 --port 8000

test:
	cd server && ../$(VENV)/bin/python -m pytest -q

lint:
	cd server && ../$(VENV)/bin/ruff check .

coverage:
	cd server && PYTHONPATH=. ../$(VENV)/bin/python -m pytest -q --cov=app --cov-report=term-missing

firmware:
	. $(IDF_EXPORT) && cd firmware && idf.py reconfigure \
	  && ../scripts/patch_bsp_display.sh && idf.py build

flash:
	. $(IDF_EXPORT) && cd firmware && idf.py -p $(PORT) flash
