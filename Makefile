.PHONY: test install-deps

install-deps:
	pip install -r requirements.txt

test:
	PYTHONPATH=. python -m pytest tests/ -v 