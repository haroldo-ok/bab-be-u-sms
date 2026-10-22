# Bab Be U SMS - banked devkitSMS build.
# Data banks in gen/ are produced by tools/convert.py from the upstream
# Love2D game.  Run:  python3 tools/convert.py /path/to/bab-be-u
DEVKIT   ?= $(HOME)/.devkitsms
PROG     ?= babbeu

CC       := sdcc
AS       := sdasz80
IHX2SMS  := ihx2sms

include gen/banks.mk

CFLAGS   := -mz80 -I$(DEVKIT)/include --peep-file $(DEVKIT)/include/peep-rules.txt -Isrc -Igen --opt-code-size --max-allocs-per-node 3000
LDFLAGS  := -mz80 --no-std-crt0 --data-loc 0xC000 -Wl-u $(BANKFLAGS) $(CODEBANKFLAGS)

CODEOBJS := main.rel engine.rel parsebank.rel movbank.rel render.rel audio.rel bankio.rel propfast.rel gamedata.rel

all: $(PROG).sms

%.rel: src/%.c src/engine.h gen/gamedata.h
	$(CC) $(CFLAGS) -c $< -o $@

# banked code (executed from slot2 banks; see gen/banks.mk CODEBANKFLAGS)
parsebank.rel: src/parsebank.c src/engine.h gen/gamedata.h
	$(CC) $(CFLAGS) --codeseg BANKP --constseg BANKP -c $< -o $@

movbank.rel: src/movbank.c src/engine.h gen/gamedata.h
	$(CC) $(CFLAGS) --codeseg BANKM --constseg BANKM -c $< -o $@

gamedata.rel: gen/gamedata.c gen/gamedata.h
	$(CC) $(CFLAGS) -c $< -o $@

propfast.rel: src/propfast.s
	$(AS) -l -o $@ $<

banks.rel: gen/banks.s $(wildcard gen/bank*.bin)
	$(AS) -l -o $@ $<

.PHONY: $(PROG).ihx
$(PROG).ihx: $(CODEOBJS) banks.rel
	$(CC) -o $@ $(LDFLAGS) $(DEVKIT)/lib/crt0_sms.rel banks.rel $(CODEOBJS) \
	      $(DEVKIT)/lib/SMSlib.lib $(DEVKIT)/lib/PSGlib.lib 2>&1 | tee link.log
	@if grep -q "Undefined Global" link.log; then echo "LINK FAILED: undefined symbols (see link.log)"; exit 1; fi
	@test -s $@

$(PROG).sms: $(PROG).ihx
	$(IHX2SMS) $< $@

convert:
	python3 tools/convert.py $(BABSRC)

playtest: $(PROG).sms
	@mkdir -p shots
	smstest $(PROG).sms tests/playtest.txt

clean:
	rm -f *.rel *.ihx *.sym *.lst *.map *.noi *.lk *.asm *.rst $(PROG).sms

.PHONY: all clean convert playtest
