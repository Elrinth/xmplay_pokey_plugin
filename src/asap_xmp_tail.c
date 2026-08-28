/* Concatenated after official asap.c at compile time (see Makefile).
 * Sees ASAP / Pokey internals so we can dump 18-byte register snapshots
 * without generating PCM. Public ASAP does not expose this.
 */
int ASAP_XmpDo6502Frame(ASAP *self)
{
	return ASAP_Do6502Frame(self);
}

int ASAP_XmpStorePokeyRegs(const ASAP *self, unsigned char *out18)
{
	int silence = 1;
	int chip, i;
	const Pokey *pokey;
	unsigned char *p;

	if (!self || !out18)
		return 1;
	for (chip = 0; chip < 2; chip++) {
		pokey = (chip == 0) ? &self->pokeys.basePokey : &self->pokeys.extraPokey;
		p = out18 + chip * 9;
		for (i = 0; i < 4; i++) {
			if ((pokey->channels[i].audc & 0xf) != 0) {
				silence = 0;
				p[i * 2] = (unsigned char) pokey->channels[i].audf;
				p[i * 2 + 1] = (unsigned char) pokey->channels[i].audc;
			} else {
				p[i * 2] = 0;
				p[i * 2 + 1] = 0;
			}
		}
		p[8] = (unsigned char) pokey->audctl;
	}
	return silence;
}
