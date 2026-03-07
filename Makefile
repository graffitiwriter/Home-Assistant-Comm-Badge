.DEFAULT_GOAL := help

help:
	@echo "Available commands:"
	@echo "  make preview       - Sync CSS from .ino into preview.html"
	@echo "  make preview-open  - Sync CSS from .ino into preview.html and opens it"

preview:
	python3 scripts/update_preview.py
preview-open:
	python3 scripts/update_preview.py
	open preview.html
