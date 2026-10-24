SHELL := /bin/bash
PY ?= .venv/bin/python
VERSION := 1.0.0

# devkitARM in its container: the app, the fonts' bcfnt, nothing installed on the host.
DKA := docker run --rm --user "$$(id -u):$$(id -g)" -v "$$PWD:/w" -w /w devkitpro/devkitarm

# makerom (3DSGuy/Project_CTR) and bannertool (carstene1ns/3ds-bannertool) are not in the
# devkitARM image: point these at local builds (the 3DS UA repo keeps both).
MAKEROM ?= ../3ds-ua/tmp/tools/makerom/makerom
BANNERTOOL ?= ../3ds-ua/tmp/tools/bannertool.bin

FONT := app/assets/fonts/Nunito[wght].ttf
CIA := FriendAutoAddTool-$(VERSION).cia

.PHONY: help venv fonts art app cia makerom test clean

help: ## show this list
	@grep -E '^[a-z-]+:.*?##' $(MAKEFILE_LIST) | sed 's/:.*##/\t—/'

venv: ## the Python tools' environment (fontTools, Pillow) -> .venv
	python3 -m venv .venv && .venv/bin/pip install -q fonttools pillow

# Nunito (OFL) at 700 and 800, cut to the characters the app draws, as bcfnt for citro2d -
# the same set as the 3DS UA installer. Two sizes: 24 px for headings and 12 px for small
# text, which the 24 px glyphs, halved without mipmaps, render broken. The bcfnt files are
# committed; rebuild after adding a character outside the set.
fonts: ## the UI fonts -> app/romfs/gfx (needs .venv + docker)
	@mkdir -p build/fonts app/romfs/gfx
	for w in 700 800; do $(PY) -m fontTools.varLib.instancer "$(FONT)" wght=$$w -q -o build/fonts/Nunito-$$w.ttf || exit 1; done
	$(PY) -c "cps = [*range(0x20, 0x7F), *range(0xA0, 0x100), *range(0x400, 0x460), 0x490, 0x491, 0x2BC, 0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x201E, 0x2026, 0x2116, 0x2248, 0x2192]; open('build/fonts/charset.txt', 'w').write(' '.join(map(str, cps)))"
	$(DKA) sh -c 'for w in 700 800; do \
		/opt/devkitpro/tools/bin/mkbcfnt -s 24 -w build/fonts/charset.txt -o app/romfs/gfx/nunito_$$w.bcfnt build/fonts/Nunito-$$w.ttf && \
		/opt/devkitpro/tools/bin/mkbcfnt -s 12 -w build/fonts/charset.txt -o app/romfs/gfx/nunito_$${w}_small.bcfnt build/fonts/Nunito-$$w.ttf || exit 1; done'

art: ## icon, HOME Menu banner and its sound -> app/art (after fonts)
	$(PY) tools/art.py build/fonts/Nunito-800.ttf app/art

app: ## the app -> app/FriendAutoAddTool.3dsx
	$(DKA) make -C app

# The splash is signed with the HOME Menu's key (tools/logo.py), read from the console's own
# HOME Menu code; first build with makerom's homebrew logo, which it reworks.
HOME_MENU_CODE ?= ../3ds-ua/work/0004003000009802/code.bin
cia: app ## the app as a CIA, with its HOME Menu banner and splash -> $(CIA)
	$(BANNERTOOL) makebanner -i app/art/banner.png -a app/art/banner.wav -o app/art/banner.bnr
	$(MAKE) makerom
	@if [ -f $(HOME_MENU_CODE) ]; then \
		$(PY) tools/logo.py render app/FriendAutoAddTool.cia build/fonts/Nunito-800.ttf build/logo && \
		$(DKA) sh -c 'for n in waves logo; do /opt/devkitpro/tools/bin/tex3ds -f etc1 -q high -z none -r -o build/logo/$$n.etc1 build/logo/$$n.png || exit 1; done' && \
		$(PY) tools/logo.py pack build/logo $(HOME_MENU_CODE) && \
		$(MAKE) makerom CIA_LOGO="-logo ../build/logo/logo.bcma.lz"; \
	else echo "no $(HOME_MENU_CODE): the CIA keeps makerom's homebrew splash"; fi
	cp app/FriendAutoAddTool.cia $(CIA)
	@ls -la $(CIA)

makerom:
	cd app && ../$(MAKEROM) -f cia -o FriendAutoAddTool.cia -target t -exefslogo $(CIA_LOGO) -rsf cia.rsf \
		-elf FriendAutoAddTool.elf -icon FriendAutoAddTool.smdh -banner art/banner.bnr \
		-major $(word 1,$(subst ., ,$(VERSION))) -minor $(word 2,$(subst ., ,$(VERSION))) -micro $(word 3,$(subst ., ,$(VERSION)))

test: ## host tests of friend codes, the base parser and the planner
	@mkdir -p build
	cc -std=c99 -O2 -Wall -Wextra -Werror -o build/core_test app/tests/core_test.c app/source/fc.c app/source/db.c app/source/planner.c
	build/core_test

clean: ## remove the build output
	$(DKA) make -C app clean
	rm -rf build app/art/banner.bnr app/FriendAutoAddTool.cia $(CIA)
