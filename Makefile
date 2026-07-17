# Version from file
VERSION := $(shell cat version.txt)

## Tag this version
.PHONY: tag
tag:
	@if git rev-parse -q --verify "refs/tags/$(VERSION)" >/dev/null; then \
		echo "ERROR: tag $(VERSION) already exists"; exit 1; \
	fi
	@if ! grep -q "^## $(VERSION)" CHANGELOG.md; then \
		echo "ERROR: no '## $(VERSION)' section in CHANGELOG.md"; exit 1; \
	fi
	git tag -a $(VERSION) -m "Release $(VERSION)" && git push origin $(VERSION) && \
	echo "Tagged: $(VERSION)"
