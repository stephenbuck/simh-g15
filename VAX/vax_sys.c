/* vax_sys.c: VAX simulator interface

   Copyright (c) 1998-2004, Robert M Supnik

   Permission is hereby granted, free of charge, to any person obtaining a
   copy of this software and associated documentation files (the "Software"),
   to deal in the Software without restriction, including without limitation
   the rights to use, copy, modify, merge, publish, distribute, sublicense,
   and/or sell copies of the Software, and to permit persons to whom the
   Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
   ROBERT M SUPNIK BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
   IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

   Except as contained in this notice, the name of Robert M Supnik shall not
   be used in advertising or otherwise to promote the sale, use or other dealings
   in this Software without prior written authorization from Robert M Supnik.

   13-Jul-04	RMS	Fixed bad block routine
   16-Jun-04	RMS	Added DHQ11 support
   21-Mar-04	RMS	Added RXV21 support
   06-May-03	RMS	Added support for second DELQA
   12-Oct-02	RMS	Added multiple RQ controller support
   10-Oct-02	RMS	Added DELQA support
   21-Sep-02	RMS	Extended symbolic ex/mod to all byte devices
   06-Sep-02	RMS	Added TMSCP support
   14-Jul-02	RMS	Added infinite loop message
*/

#include "vax_defs.h"
#include <ctype.h>

extern DEVICE cpu_dev;
extern DEVICE tlb_dev;
extern DEVICE rom_dev;
extern DEVICE nvr_dev;
extern DEVICE sysd_dev;
extern DEVICE qba_dev;
extern DEVICE ptr_dev, ptp_dev;
extern DEVICE tti_dev, tto_dev;
extern DEVICE csi_dev, cso_dev;
extern DEVICE lpt_dev;
extern DEVICE clk_dev;
extern DEVICE rq_dev, rqb_dev, rqc_dev, rqd_dev;
extern DEVICE rl_dev;
extern DEVICE ry_dev;
extern DEVICE ts_dev;
extern DEVICE tq_dev;
extern DEVICE dz_dev;
extern DEVICE vh_dev;
extern DEVICE xq_dev, xqb_dev;
extern UNIT cpu_unit;
extern REG cpu_reg[];
extern uint32 *M;
extern int32 saved_PC;
extern int32 sim_switches;

extern void WriteB (int32 pa, int32 val);
extern void rom_wr (int32 pa, int32 val, int32 lnt);
t_stat fprint_sym_m (FILE *of, uint32 addr, t_value *val);
int32 fprint_sym_qoimm (FILE *of, t_value *val, int32 vp, int32 lnt);
t_stat parse_sym_m (char *cptr, uint32 addr, t_value *val);
int32 parse_brdisp (char *cptr, uint32 addr, t_value *val,
	int32 vp, int32 lnt, t_stat *r);
int32 parse_spec (char *cptr, uint32 addr, t_value *val,
	int32 vp, int32 disp, t_stat *r);
char *parse_rnum (char *cptr, int32 *rn);
int32 parse_sym_qoimm (int32 *lit, t_value *val, int32 vp,
	int lnt, int32 minus);

/* SCP data structures and interface routines

   sim_name		simulator name string
   sim_PC		pointer to saved PC register descriptor
   sim_emax		number of words for examine
   sim_devices		array of pointers to simulated devices
   sim_stop_messages	array of pointers to stop messages
   sim_load		binary loader
*/

char sim_name[] = "VAX";

REG *sim_PC = &cpu_reg[0];

int32 sim_emax = 60;

DEVICE *sim_devices[] = { 
	&cpu_dev,
	&tlb_dev,
	&rom_dev,
	&nvr_dev,
	&sysd_dev,
	&qba_dev,
	&tti_dev,
	&tto_dev,
	&csi_dev,
	&cso_dev,
	&clk_dev,
	&ptr_dev,
	&ptp_dev,
	&lpt_dev,
	&dz_dev,
	&vh_dev,
	&rl_dev,
	&rq_dev,
	&rqb_dev,
	&rqc_dev,
	&rqd_dev,
	&ry_dev,
	&ts_dev,
	&tq_dev,
	&xq_dev,
	&xqb_dev,
	NULL };

const char *sim_stop_messages[] = {
	"Unknown error",
	"HALT instruction",
	"Breakpoint",
	"CHMx on interrupt stack",
	"Invalid SCB vector",
	"Exception in interrupt or exception",
	"Process PTE in P0 or P1 space",
	"Interrupt at undefined IPL",
	"Fatal RQDX3 error",
	"Infinite loop",
	"Sanity timer expired",
	"Unknown error",
	"Unknown abort code" };

/* Binary loader

   The binary loader handles absolute system images, that is, system
   images linked /SYSTEM.  These are simply a byte stream, with no
   origin or relocation information.

   -r		load ROM
   -n		load NVR
   -o		for memory, specify origin
*/

t_stat sim_load (FILE *fileref, char *cptr, char *fnam, int flag)
{
t_stat r;
int32 i;
uint32 origin, limit;
extern int32 ssc_cnf;
#define SSCCNF_BLO	0x80000000

if (flag) return SCPE_ARG;				/* dump? */
if (sim_switches & SWMASK ('R')) {			/* ROM? */
	origin = ROMBASE;
	limit = ROMBASE + ROMSIZE;  }
else if (sim_switches & SWMASK ('N')) {			/* NVR? */
	origin = NVRBASE;
	limit = NVRBASE + NVRSIZE;
	ssc_cnf = ssc_cnf & ~SSCCNF_BLO;  }
else {	origin = 0;					/* memory */
	limit = (uint32) cpu_unit.capac;
	if (sim_switches & SWMASK ('O')) {		/* origin? */
	    origin = (int32) get_uint (cptr, 16, 0xFFFFFFFF, &r);
	    if (r != SCPE_OK) return SCPE_ARG;  }  }

while ((i = getc (fileref)) != EOF) {			/* read byte stream */
	if (origin >= limit) return SCPE_NXM;		/* NXM? */
	if (sim_switches & SWMASK ('R'))		/* ROM? */
	    rom_wr (origin, i, L_BYTE);			/* not writeable */
	else WriteB (origin, i);			/* store byte */
	origin = origin + 1;  }
return SCPE_OK;
}

/* Factory bad block table creation routine

   This routine writes a DEC standard 044 compliant bad block table on the
   last track of the specified unit.  The bad block table consists of 10
   repetitions of the same table, formatted as follows:

	words 0-1	pack id number
	words 2-3	cylinder/sector/surface specifications
	 :
	words n-n+1	end of table (-1,-1)

   Inputs:
	uptr	=	pointer to unit
	sec	=	number of sectors per surface
	wds	=	number of words per sector
   Outputs:
	sta	=	status code
*/

t_stat pdp11_bad_block (UNIT *uptr, int32 sec, int32 wds)
{
int32 i;
t_addr da;
uint16 *buf;

if ((sec < 2) || (wds < 16)) return SCPE_ARG;
if ((uptr->flags & UNIT_ATT) == 0) return SCPE_UNATT;
if (!get_yn ("Overwrite last track? [N]", FALSE)) return SCPE_OK;
da = (uptr->capac - (sec * wds)) * sizeof (uint16);
if (sim_fseek (uptr->fileref, da, SEEK_SET)) return SCPE_IOERR;
if ((buf = malloc (wds * sizeof (uint16))) == NULL) return SCPE_MEM;
buf[0] = 0x1234;
buf[1] = 0x5678;
buf[2] = buf[3] = 0;
for (i = 4; i < wds; i++) buf[i] = 0xFFFF;
for (i = 0; (i < sec) && (i < 10); i++)
	sim_fwrite (buf, sizeof (uint16), wds, uptr->fileref);
free (buf);
if (ferror (uptr->fileref)) return SCPE_IOERR;
return SCPE_OK;
}

/* Dispatch/decoder table

   The first entry contains:
	- FPD legal flag (DR_F)
	- number of specifiers for decode bits 2:0>
	- number of specifiers for unimplemented instructions bits<6:4>
 */

const uint16 drom[NUM_INST][MAX_SPEC + 1] = {
0,	0,	0,	0,	0,	0,	0,		/* HALT */
0,	0,	0,	0,	0,	0,	0,		/* NOP */
0,	0,	0,	0,	0,	0,	0,		/* REI */
0,	0,	0,	0,	0,	0,	0,		/* BPT */
0,	0,	0,	0,	0,	0,	0,		/* RET */
0,	0,	0,	0,	0,	0,	0,		/* RSB */
0,	0,	0,	0,	0,	0,	0,		/* LDPCTX */
0,	0,	0,	0,	0,	0,	0,		/* SVPCTX */
4+DR_F,	RW,	AB,	RW,	AB,	0,	0,		/* CVTPS */
4+DR_F,	RW,	AB,	RW,	AB,	0,	0,		/* CVTSP */
6,	RL,	RL,	RL,	RL,	RL,	WL,		/* INDEX */
4+DR_F,	AB,	RL,	RW,	AB,	0,	0,		/* CRC */
3,	RB,	RW,	AB,	0,	0,	0,		/* PROBER */
3,	RB,	RW,	AB,	0,	0,	0,		/* PROBEW */
2,	AB,	AB,	0,	0,	0,	0,		/* INSQUE */
2,	AB,	WL,	0,	0,	0,	0,		/* REMQUE */
1,	BB,	0,	0,	0,	0,	0,		/* BSBB */
1,	BB,	0,	0,	0,	0,	0,		/* BRB */
1,	BB,	0,	0,	0,	0,	0,		/* BNEQ */
1,	BB,	0,	0,	0,	0,	0,		/* BEQL */
1,	BB,	0,	0,	0,	0,	0,		/* BGTR */
1,	BB,	0,	0,	0,	0,	0,		/* BLEQ */
1,	AB,	0,	0,	0,	0,	0,		/* JSB */
1,	AB,	0,	0,	0,	0,	0,		/* JMP */
1,	BB,	0,	0,	0,	0,	0,		/* BGEQ */
1,	BB,	0,	0,	0,	0,	0,		/* BLSS */
1,	BB,	0,	0,	0,	0,	0,		/* BGTRU */
1,	BB,	0,	0,	0,	0,	0,		/* BLEQU */
1,	BB,	0,	0,	0,	0,	0,		/* BVC */
1,	BB,	0,	0,	0,	0,	0,		/* BVS */
1,	BB,	0,	0,	0,	0,	0,		/* BCC */
1,	BB,	0,	0,	0,	0,	0,		/* BCS */
4+DR_F,	RW,	AB,	RW,	AB,	0,	0,		/* ADDP4 */
6+DR_F,	RW,	AB,	RW,	AB,	RW,	AB,		/* ADDP6 */
4+DR_F,	RW,	AB,	RW,	AB,	0,	0,		/* SUBP4 */
6+DR_F,	RW,	AB,	RW,	AB,	RW,	AB,		/* SUBP6 */
5+DR_F,	RW,	AB,	AB,	RW,	AB,	0,		/* CVTPT */
6+DR_F,	RW,	AB,	RW,	AB,	RW,	AB,		/* MULP6 */
5+DR_F,	RW,	AB,	AB,	RW,	AB,	0,		/* CVTTP */
6+DR_F,	RW,	AB,	RW,	AB,	RW,	AB,		/* DIVP6 */
3+DR_F,	RW,	AB,	AB,	0,	0,	0,		/* MOVC3 */
3+DR_F,	RW,	AB,	AB,	0,	0,	0,		/* CMPC3 */
4+DR_F,	RW,	AB,	AB,	RB,	0,	0,		/* SCANC */
4+DR_F,	RW,	AB,	AB,	RB,	0,	0,		/* SPANC */
5+DR_F,	RW,	AB,	RB,	RW,	AB,	0,		/* MOVC5 */
5+DR_F,	RW,	AB,	RB,	RW,	AB,	0,		/* CMPC5 */
6+DR_F,	RW,	AB,	RB,	AB,	RW,	AB,		/* MOVTC */
6+DR_F,	RW,	AB,	RB,	AB,	RW,	AB,		/* MOVTUC */
1,	BW,	0,	0,	0,	0,	0,		/* BSBW */
1,	BW,	0,	0,	0,	0,	0,		/* BRW */
2,	RW,	WL,	0,	0,	0,	0,		/* CVTWL */
2,	RW,	WB,	0,	0,	0,	0,		/* CVTWB */
3+DR_F,	RW,	AB,	AB,	0,	0,	0,		/* MOVP */
3+DR_F,	RW,	AB,	AB,	0,	0,	0,		/* CMPP3 */
3+DR_F,	RW,	AB,	WL,	0,	0,	0,		/* CVTPL */
4+DR_F,	RW,	AB,	RW,	AB,	0,	0,		/* CMPP4 */
4+DR_F,	RW,	AB,	AB,	AB,	0,	0,		/* EDITPC */
4+DR_F,	RW,	AB,	RW,	AB,	0,	0,		/* MATCHC */
3+DR_F,	RB,	RW,	AB,	0,	0,	0,		/* LOCC */
3+DR_F,	RB,	RW,	AB,	0,	0,	0,		/* SKPC */
2,	RW,	WL,	0,	0,	0,	0,		/* MOVZWL */
4,	RW,	RW,	MW,	BW,	0,	0,		/* ACBW */
2,	AW,	WL,	0,	0,	0,	0,		/* MOVAW */
1,	AW,	0,	0,	0,	0,	0,		/* PUSHAW */
2,	RF,	ML,	0,	0,	0,	0,		/* ADDF2 */
3,	RF,	RF,	WL,	0,	0,	0,		/* ADDF3 */
2,	RF,	ML,	0,	0,	0,	0,		/* SUBF2 */
3,	RF,	RF,	WL,	0,	0,	0,		/* SUBF3 */
2,	RF,	ML,	0,	0,	0,	0,		/* MULF2 */
3,	RF,	RF,	WL,	0,	0,	0,		/* MULF3 */
2,	RF,	ML,	0,	0,	0,	0,		/* DIVF2 */
3,	RF,	RF,	WL,	0,	0,	0,		/* DIVF3 */
2,	RF,	WB,	0,	0,	0,	0,		/* CVTFB */
2,	RF,	WW,	0,	0,	0,	0,		/* CVTFW */
2,	RF,	WL,	0,	0,	0,	0,		/* CVTFL */
2,	RF,	WL,	0,	0,	0,	0,		/* CVTRFL */
2,	RB,	WL,	0,	0,	0,	0,		/* CVTBF */
2,	RW,	WL,	0,	0,	0,	0,		/* CVTWF */
2,	RL,	WL,	0,	0,	0,	0,		/* CVTLF */
4,	RF,	RF,	ML,	BW,	0,	0,		/* ACBF */
2,	RF,	WL,	0,	0,	0,	0,		/* MOVF */
2,	RF,	RF,	0,	0,	0,	0,		/* CMPF */
2,	RF,	WL,	0,	0,	0,	0,		/* MNEGF */
1,	RF,	0,	0,	0,	0,	0,		/* TSTF */
5,	RF,	RB,	RF,	WL,	WL,	0,		/* EMODF */
3,	RF,	RW,	AB,	0,	0,	0,		/* POLYF */
2,	RF,	WQ,	0,	0,	0,	0,		/* CVTFD */
0,	0,	0,	0,	0,	0,	0,		/* reserved */
2,	RW,	WW,	0,	0,	0,	0,		/* ADAWI */
0,	0,	0,	0,	0,	0,	0,		/* reserved */
0,	0,	0,	0,	0,	0,	0,		/* reserved */
0,	0,	0,	0,	0,	0,	0,		/* reserved */
2,	AB,	AQ,	0,	0,	0,	0,		/* INSQHI */
2,	AB,	AQ,	0,	0,	0,	0,		/* INSQTI */
2,	AQ,	WL,	0,	0,	0,	0,		/* REMQHI */
2,	AQ,	WL,	0,	0,	0,	0,		/* REMQTI */
2,	RD,	MQ,	0,	0,	0,	0,		/* ADDD2 */
3,	RD,	RD,	WQ,	0,	0,	0,		/* ADDD3 */
2,	RD,	MQ,	0,	0,	0,	0,		/* SUBD2 */
3,	RD,	RD,	WQ,	0,	0,	0,		/* SUBD3 */
2,	RD,	MQ,	0,	0,	0,	0,		/* MULD2 */
3,	RD,	RD,	WQ,	0,	0,	0,		/* MULD3 */
2,	RD,	MQ,	0,	0,	0,	0,		/* DIVD2 */
3,	RD,	RD,	WQ,	0,	0,	0,		/* DIVD3 */
2,	RD,	WB,	0,	0,	0,	0,		/* CVTDB */
2,	RD,	WW,	0,	0,	0,	0,		/* CVTDW */
2,	RD,	WL,	0,	0,	0,	0,		/* CVTDL */
2,	RD,	WL,	0,	0,	0,	0,		/* CVTRDL */
2,	RB,	WQ,	0,	0,	0,	0,		/* CVTBD */
2,	RW,	WQ,	0,	0,	0,	0,		/* CVTWD */
2,	RL,	WQ,	0,	0,	0,	0,		/* CVTLD */
4,	RD,	RD,	MQ,	BW,	0,	0,		/* ACBD */
2,	RD,	WQ,	0,	0,	0,	0,		/* MOVD */
2,	RD,	RD,	0,	0,	0,	0,		/* CMPD */
2,	RD,	WQ,	0,	0,	0,	0,		/* MNEGD */
1,	RD,	0,	0,	0,	0,	0,		/* TSTD */
5,	RD,	RB,	RD,	WL,	WQ,	0,		/* EMODD */
3,	RD,	RW,	AB,	0,	0,	0,		/* POLYD */
2,	RD,	WL,	0,	0,	0,	0,		/* CVTDF */
0,	0,	0,	0,	0,	0,	0,		/* reserved */
3,	RB,	RL,	WL,	0,	0,	0,		/* ASHL */
3,	RB,	RQ,	WQ,	0,	0,	0,		/* ASHQ */
4,	RL,	RL,	RL,	WQ,	0,	0,		/* EMUL */
4,	RL,	RQ,	WL,	WL,	0,	0,		/* EDIV */
1,	WQ,	0,	0,	0,	0,	0,		/* CLRQ */
2,	RQ,	WQ,	0,	0,	0,	0,		/* MOVQ */
2,	AQ,	WL,	0,	0,	0,	0,		/* MOVAQ */
1,	AQ,	0,	0,	0,	0,	0,		/* PUSHAQ */
2,	RB,	MB,	0,	0,	0,	0,		/* ADDB2 */
3,	RB,	RB,	WB,	0,	0,	0,		/* ADDB3 */
2,	RB,	MB,	0,	0,	0,	0,		/* SUBB2 */
3,	RB,	RB,	WB,	0,	0,	0,		/* SUBB3 */
2,	RB,	MB,	0,	0,	0,	0,		/* MULB2 */
3,	RB,	RB,	WB,	0,	0,	0,		/* MULB3 */
2,	RB,	MB,	0,	0,	0,	0,		/* DIVB2 */
3,	RB,	RB,	WB,	0,	0,	0,		/* DIVB3 */
2,	RB,	MB,	0,	0,	0,	0,		/* BISB2 */
3,	RB,	RB,	WB,	0,	0,	0,		/* BISB3 */
2,	RB,	MB,	0,	0,	0,	0,		/* BICB2 */
3,	RB,	RB,	WB,	0,	0,	0,		/* BICB3 */
2,	RB,	MB,	0,	0,	0,	0,		/* XORB2 */
3,	RB,	RB,	WB,	0,	0,	0,		/* XORB3 */
2,	RB,	WB,	0,	0,	0,	0,		/* MNEGB */
3,	RB,	RB,	RB,	0,	0,	0,		/* CASEB */
2,	RB,	WB,	0,	0,	0,	0,		/* MOVB */
2,	RB,	RB,	0,	0,	0,	0,		/* CMPB */
2,	RB,	WB,	0,	0,	0,	0,		/* MCOMB */
2,	RB,	RB,	0,	0,	0,	0,		/* BITB */
1,	WB,	0,	0,	0,	0,	0,		/* CLRB */
1,	RB,	0,	0,	0,	0,	0,		/* TSTB */
1,	MB,	0,	0,	0,	0,	0,		/* INCB */
1,	MB,	0,	0,	0,	0,	0,		/* DECB */
2,	RB,	WL,	0,	0,	0,	0,		/* CVTBL */
2,	RB,	WW,	0,	0,	0,	0,		/* CVTBW */
2,	RB,	WL,	0,	0,	0,	0,		/* MOVZBL */
2,	RB,	WW,	0,	0,	0,	0,		/* MOVZBW */
3,	RB,	RL,	WL,	0,	0,	0,		/* ROTL */
4,	RB,	RB,	MB,	BW,	0,	0,		/* ACBB */
2,	AB,	WL,	0,	0,	0,	0,		/* MOVAB */
1,	AB,	0,	0,	0,	0,	0,		/* PUSHAB */
2,	RW,	MW,	0,	0,	0,	0,		/* ADDW2 */
3,	RW,	RW,	WW,	0,	0,	0,		/* ADDW3 */
2,	RW,	MW,	0,	0,	0,	0,		/* SUBW2 */
3,	RW,	RW,	WW,	0,	0,	0,		/* SUBW3 */
2,	RW,	MW,	0,	0,	0,	0,		/* MULW2 */
3,	RW,	RW,	WW,	0,	0,	0,		/* MULW3 */
2,	RW,	MW,	0,	0,	0,	0,		/* DIVW2 */
3,	RW,	RW,	WW,	0,	0,	0,		/* DIVW3 */
2,	RW,	MW,	0,	0,	0,	0,		/* BISW2 */
3,	RW,	RW,	WW,	0,	0,	0,		/* BISW3 */
2,	RW,	MW,	0,	0,	0,	0,		/* BICW2 */
3,	RW,	RW,	WW,	0,	0,	0,		/* BICW3 */
2,	RW,	MW,	0,	0,	0,	0,		/* XORW2 */
3,	RW,	RW,	WW,	0,	0,	0,		/* XORW3 */
2,	RW,	WW,	0,	0,	0,	0,		/* MNEGW */
3,	RW,	RW,	RW,	0,	0,	0,		/* CASEW */
2,	RW,	WW,	0,	0,	0,	0,		/* MOVW */
2,	RW,	RW,	0,	0,	0,	0,		/* CMPW */
2,	RW,	WW,	0,	0,	0,	0,		/* MCOMW */
2,	RW,	RW,	0,	0,	0,	0,		/* BITW */
1,	WW,	0,	0,	0,	0,	0,		/* CLRW */
1,	RW,	0,	0,	0,	0,	0,		/* TSTW */
1,	MW,	0,	0,	0,	0,	0,		/* INCW */
1,	MW,	0,	0,	0,	0,	0,		/* DECW */
1,	RW,	0,	0,	0,	0,	0,		/* BISPSW */
1,	RW,	0,	0,	0,	0,	0,		/* BICPSW */
1,	RW,	0,	0,	0,	0,	0,		/* POPR */
1,	RW,	0,	0,	0,	0,	0,		/* PUSHR */
1,	RW,	0,	0,	0,	0,	0,		/* CHMK */
1,	RW,	0,	0,	0,	0,	0,		/* CHME */
1,	RW,	0,	0,	0,	0,	0,		/* CHMS */
1,	RW,	0,	0,	0,	0,	0,		/* CHMU */
2,	RL,	ML,	0,	0,	0,	0,		/* ADDL2 */
3,	RL,	RL,	WL,	0,	0,	0,		/* ADDL3 */
2,	RL,	ML,	0,	0,	0,	0,		/* SUBL2 */
3,	RL,	RL,	WL,	0,	0,	0,		/* SUBL3 */
2,	RL,	ML,	0,	0,	0,	0,		/* MULL2 */
3,	RL,	RL,	WL,	0,	0,	0,		/* MULL3 */
2,	RL,	ML,	0,	0,	0,	0,		/* DIVL2 */
3,	RL,	RL,	WL,	0,	0,	0,		/* DIVL3 */
2,	RL,	ML,	0,	0,	0,	0,		/* BISL2 */
3,	RL,	RL,	WL,	0,	0,	0,		/* BISL3 */
2,	RL,	ML,	0,	0,	0,	0,		/* BICL2 */
3,	RL,	RL,	WL,	0,	0,	0,		/* BICL3 */
2,	RL,	ML,	0,	0,	0,	0,		/* XORL2 */
3,	RL,	RL,	WL,	0,	0,	0,		/* XORL3 */
2,	RL,	WL,	0,	0,	0,	0,		/* MNEGL */
3,	RL,	RL,	RL,	0,	0,	0,		/* CASEL */
2,	RL,	WL,	0,	0,	0,	0,		/* MOVL */
2,	RL,	RL,	0,	0,	0,	0,		/* CMPL */
2,	RL,	WL,	0,	0,	0,	0,		/* MCOML */
2,	RL,	RL,	0,	0,	0,	0,		/* BITL */
1,	WL,	0,	0,	0,	0,	0,		/* CLRL */
1,	RL,	0,	0,	0,	0,	0,		/* TSTL */
1,	ML,	0,	0,	0,	0,	0,		/* INCL */
1,	ML,	0,	0,	0,	0,	0,		/* DECL */
2,	RL,	ML,	0,	0,	0,	0,		/* ADWC */
2,	RL,	ML,	0,	0,	0,	0,		/* SBWC */
2,	RL,	RL,	0,	0,	0,	0,		/* MTPR */
2,	RL,	WL,	0,	0,	0,	0,		/* MFPR */
1,	WL,	0,	0,	0,	0,	0,		/* MOVPSL */
1,	RL,	0,	0,	0,	0,	0,		/* PUSHL */
2,	AL,	WL,	0,	0,	0,	0,		/* MOVAL */
1,	AL,	0,	0,	0,	0,	0,		/* PUSHAL */
3,	RL,	VB,	BB,	0,	0,	0,		/* BBS */
3,	RL,	VB,	BB,	0,	0,	0,		/* BBC */
3,	RL,	VB,	BB,	0,	0,	0,		/* BBSS */
3,	RL,	VB,	BB,	0,	0,	0,		/* BBCS */
3,	RL,	VB,	BB,	0,	0,	0,		/* BBSC */
3,	RL,	VB,	BB,	0,	0,	0,		/* BBCC */
3,	RL,	VB,	BB,	0,	0,	0,		/* BBSSI */
3,	RL,	VB,	BB,	0,	0,	0,		/* BBCCI */
2,	RL,	BB,	0,	0,	0,	0,		/* BLBS */
2,	RL,	BB,	0,	0,	0,	0,		/* BLBC */
4,	RL,	RB,	VB,	WL,	0,	0,		/* FFS */
4,	RL,	RB,	VB,	WL,	0,	0,		/* FFC */
4,	RL,	RB,	VB,	RL,	0,	0,		/* CMPV */
4,	RL,	RB,	VB,	RL,	0,	0,		/* CMPZV */
4,	RL,	RB,	VB,	WL,	0,	0,		/* EXTV */
4,	RL,	RB,	VB,	WL,	0,	0,		/* EXTZV */
4,	RL,	RL,	RB,	VB,	0,	0,		/* INSV */
4,	RL,	RL,	ML,	BW,	0,	0,		/* ACBL */
3,	RL,	ML,	BB,	0,	0,	0,		/* AOBLSS */
3,	RL,	ML,	BB,	0,	0,	0,		/* AOBLEQ */
2,	ML,	BB,	0,	0,	0,	0,		/* SOBGEQ */
2,	ML,	BB,	0,	0,	0,	0,		/* SOBGTR */
2,	RL,	WB,	0,	0,	0,	0,		/* CVTLB */
2,	RL,	WW,	0,	0,	0,	0,		/* CVTLW */
6+DR_F,	RB,	RW,	AB,	RB,	RW,	AB,		/* ASHP */
3+DR_F,	RL,	RW,	AB,	0,	0,	0,		/* CVTLP */
2,	AB,	AB,	0,	0,	0,	0,		/* CALLG */
2,	RL,	AB,	0,	0,	0,	0,		/* CALLS */
0,	0,	0,	0,	0,	0,	0,		/* XFC */
0,	0,	0,	0,	0,	0,	0,		/* 0FD */
0,	0,	0,	0,	0,	0,	0,		/* 0FE */
0,	0,	0,	0,	0,	0,	0,		/* 0FF */
0,	0,	0,	0,	0,	0,	0,		/* 100-10F */
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,		/* 110-11F */
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,		/* 120-12F */
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,		/* 130-13F */
0,	0,	0,	0,	0,	0,	0,
0x20,	RD,	OC,	0,	0,	0,	0,		/* CVTDH */
2,	RG,	WL,	0,	0,	0,	0,		/* CVTGF */
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
2,	RG,	MQ,	0,	0,	0,	0,		/* ADDG2 */
3,	RG,	RG,	WQ,	0,	0,	0,		/* ADDG3 */
2,	RG,	MQ,	0,	0,	0,	0,		/* SUBG2 */
3,	RG,	RG,	WQ,	0,	0,	0,		/* SUBG3 */
2,	RG,	MQ,	0,	0,	0,	0,		/* MULG2 */
3,	RG,	RG,	WQ,	0,	0,	0,		/* MULG3 */
2,	RG,	MQ,	0,	0,	0,	0,		/* DIVG2 */
3,	RG,	RG,	WQ,	0,	0,	0,		/* DIVG3 */
2,	RG,	WB,	0,	0,	0,	0,		/* CVTGB */
2,	RG,	WW,	0,	0,	0,	0,		/* CVTGW */
2,	RG,	WL,	0,	0,	0,	0,		/* CVTGL */
2,	RG,	WL,	0,	0,	0,	0,		/* CVTRGL */
2,	RB,	WQ,	0,	0,	0,	0,		/* CVTBG */
2,	RW,	WQ,	0,	0,	0,	0,		/* CVTWG */
2,	RL,	WQ,	0,	0,	0,	0,		/* CVTLG */
4,	RG,	RG,	MQ,	BW,	0,	0,		/* ACBG */
2,	RG,	WQ,	0,	0,	0,	0,		/* MOVG */
2,	RG,	RG,	0,	0,	0,	0,		/* CMPG */
2,	RG,	WQ,	0,	0,	0,	0,		/* MNEGG */
1,	RG,	0,	0,	0,	0,	0,		/* TSTG */
5,	RG,	RW,	RG,	WL,	WQ,	0,		/* EMODG */
3,	RG,	RW,	AB,	0,	0,	0,		/* POLYG */
0x20,	RG,	OC,	0,	0,	0,	0,		/* CVTGH */
0,	0,	0,	0,	0,	0,	0,		/* reserved */
0,	0,	0,	0,	0,	0,	0,		/* reserved */
0,	0,	0,	0,	0,	0,	0,		/* reserved */
0,	0,	0,	0,	0,	0,	0,		/* reserved */
0,	0,	0,	0,	0,	0,	0,		/* reserved */
0,	0,	0,	0,	0,	0,	0,		/* reserved */
0,	0,	0,	0,	0,	0,	0,		/* reserved */
0,	0,	0,	0,	0,	0,	0,		/* reserved */
0,	0,	0,	0,	0,	0,	0,		/* reserved */
0x20,	OC,	OC,	0,	0,	0,	0,		/* ADDH2 */
0x30,	OC,	OC,	OC,	0,	0,	0,		/* ADDH3 */
0x20,	OC,	OC,	0,	0,	0,	0,		/* SUBH2 */
0x30,	OC,	OC,	OC,	0,	0,	0,		/* SUBH3 */
0x20,	OC,	OC,	0,	0,	0,	0,		/* MULH2 */
0x30,	OC,	OC,	OC,	0,	0,	0,		/* MULH3 */
0x20,	OC,	OC,	0,	0,	0,	0,		/* DIVH2 */
0x30,	OC,	OC,	OC,	0,	0,	0,		/* DIVH3 */
0x20,	OC,	WB,	0,	0,	0,	0,		/* CVTHB */
0x20,	OC,	WW,	0,	0,	0,	0,		/* CVTHW */
0x20,	OC,	WL,	0,	0,	0,	0,		/* CVTHL */
0x20,	OC,	WL,	0,	0,	0,	0,		/* CVTRHL */
0x20,	RB,	OC,	0,	0,	0,	0,		/* CVTBH */
0x20,	RW,	OC,	0,	0,	0,	0,		/* CVTWH */
0x20,	RL,	OC,	0,	0,	0,	0,		/* CVTLH */
0x40,	OC,	OC,	OC,	BW,	0,	0,		/* ACBH */
0x20,	OC,	OC,	0,	0,	0,	0,		/* MOVH */
0x20,	OC,	OC,	0,	0,	0,	0,		/* CMPH */
0x20,	OC,	OC,	0,	0,	0,	0,		/* MNEGH */
0x10,	OC,	0,	0,	0,	0,	0,		/* TSTH */
0x50,	OC,	RW,	OC,	WL,	OC,	0,		/* EMODH */
0x30,	OC,	RW,	AB,	0,	0,	0,		/* POLYH */
0x20,	OC,	WQ,	0,	0,	0,	0,		/* CVTHG */
0,	0,	0,	0,	0,	0,	0,		/* reserved */
0,	0,	0,	0,	0,	0,	0,		/* reserved */
0,	0,	0,	0,	0,	0,	0,		/* reserved */
0,	0,	0,	0,	0,	0,	0,		/* reserved */
0,	0,	0,	0,	0,	0,	0,		/* reserved */
0x10,	OC,	0,	0,	0,	0,	0,		/* CLRO */
0x20,	OC,	OC,	0,	0,	0,	0,		/* MOVO */
0x20,	OC,	WL,	0,	0,	0,	0,		/* MOVAO*/
0x10,	OC,	0,	0,	0,	0,	0,		/* PUSHAO*/
0,	0,	0,	0,	0,	0,	0,		/* 180-18F */
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,		/* 190-19F */
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0x20,	RF,	OC,	0,	0,	0,	0,		/* CVTFH */
2,	RF,	WQ,	0,	0,	0,	0,		/* CVTFG */
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,		/* 1A0-1AF */
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,		/* 1B0-1BF */
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,		/* 1C0-1CF */
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,		/* 1D0-1DF */
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,		/* 1E0-1EF */
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,		/* 1F0-1FF */
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0x20,	OC,	WL,	0,	0,	0,	0,		/* CVTHF */
0x20,	OC,	WQ,	0,	0,	0,	0,		/* CVTHD */
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0,
0,	0,	0,	0,	0,	0,	0  };

/* Opcode mnemonics table */

const char *opcode[] = {
"HALT", "NOP", "REI", "BPT", "RET", "RSB", "LDPCTX", "SVPCTX",
"CVTPS", "CVTSP", "INDEX", "CRC", "PROBER", "PROBEW", "INSQUE", "REMQUE",
"BSBB", "BRB", "BNEQ", "BEQL", "BGTR", "BLEQ", "JSB", "JMP",
"BGEQ", "BLSS", "BGTRU", "BLEQU", "BVC", "BVS", "BGEQU", "BLSSU",
"ADDP4", "ADDP6", "SUBP4", "SUBP6", "CVTPT", "MULP6", "CVTTP", "DIVP",
"MOVC3", "CMPC3", "SCANC", "SPANC", "MOVC5", "CMPC5", "MOVTC", "MOVTUC",
"BSBW", "BRW", "CVTWL", "CVTWB", "MOVP", "CMPP3", "CVTPL", "CMPP4",
"EDITPC", "MATCHC", "LOCC", "SKPC", "MOVZWL", "ACBW", "MOVAW", "PUSHAW",
"ADDF2", "ADDF3", "SUBF2", "SUBF3", "MULF2", "MULF3", "DIVF2", "DIVF3",
"CVTFB", "CVTFW", "CVTFL", "CVTRFL", "CVTBF", "CVTWF", "CVTLF", "ACBF",
"MOVF", "CMPF", "MNEGF", "TSTF", "EMODF", "POLYF", "CVTFD", NULL,
"ADAWI", NULL, NULL, NULL, "INSQHI", "INSQTI", "REMQHI", "REMQTI",
"ADDD2", "ADDD3", "SUBD2", "SUBD3", "MULD2", "MULD3", "DIVD2", "DIVD3",
"CVTDB", "CVTDW", "CVTDL", "CVTRDL", "CVTBD", "CVTWD", "CVTLD", "ACBD",
"MOVD", "CMPD", "MNEGD", "TSTD", "EMODD", "POLYD", "CVTDF", NULL,
"ASHL", "ASHQ", "EMUL", "EDIV", "CLRQ", "MOVQ", "MOVAQ", "PUSHAQ",
"ADDB2", "ADDB3", "SUBB2", "SUBB3", "MULB2", "MULB3", "DIVB2", "DIVB3",
"BISB2", "BISB3", "BICB2", "BICB3", "XORB2", "XORB3", "MNEGB", "CASEB",
"MOVB", "CMPB", "MCOMB", "BITB", "CLRB", "TSTB", "INCB", "DECB",
"CVTBL", "CVTBW", "MOVZBL", "MOVZBW", "ROTL", "ACBB", "MOVAB", "PUSHAB",
"ADDW2", "ADDW3", "SUBW2", "SUBW3", "MULW2", "MULW3", "DIVW2", "DIVW3",
"BISW2", "BISW3", "BICW2", "BICW3", "XORW2", "XORW3", "MNEGW", "CASEW",
"MOVW", "CMPW", "MCOMW", "BITW", "CLRW", "TSTW", "INCW", "DECW",
"BISPSW", "BICPSW", "POPR", "PUSHR", "CHMK", "CHME", "CHMS", "CHMU",
"ADDL2", "ADDL3", "SUBL2", "SUBL3", "MULL2", "MULL3", "DIVL2", "DIVL3",
"BISL2", "BISL3", "BICL2", "BICL3", "XORL2", "XORL3", "MNEGL", "CASEL",
"MOVL", "CMPL", "MCOML", "BITL", "CLRL", "TSTL", "INCL", "DECL",
"ADWC", "SBWC", "MTPR", "MFPR", "MOVPSL", "PUSHL", "MOVAL", "PUSHAL",
"BBS", "BBC", "BBSS", "BBCS", "BBSC", "BBCC", "BBSSI", "BBCCI",
"BLBS", "BLBC", "FFS", "FFC", "CMPV", "CMPZV", "EXTV", "EXTZV",
"INSV", "ACBL", "AOBLSS", "AOBLEQ", "SOBGEQ", "SOBGTR", "CVTLB", "CVTLW",
"ASHP", "CVTLP", "CALLG", "CALLS", "XFC", NULL, NULL, NULL,
NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,		/* 100 - 11F */
NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,		/* 120 - 13F */
NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
NULL, NULL, "CVTDH", "CVTGF", NULL, NULL, NULL, NULL,
NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
"ADDG2", "ADDG3", "SUBG2", "SUBG3", "MULG2", "MULG3", "DIVG2", "DIVG3",
"CVTGB", "CVTGW", "CVTGL", "CVTRGL", "CVTBG", "CVTWG", "CVTLG", "ACBG",
"MOVG", "CMPG", "MNEGG", "TSTG", "EMODG", "POLYG", "CVTGH", NULL,
NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
"ADDH2", "ADDH3", "SUBH2", "SUBH3", "MULH2", "MULH3", "DIVH2", "DIVH3",
"CVTHB", "CVTHW", "CVTHL", "CVTRHL", "CVTBH", "CVTWH", "CVTLH", "ACBH",
"MOVH", "CMPH", "MNEGH", "TSTH", "EMODH", "POLYH", "CVTHG", NULL,
NULL, NULL, NULL, NULL, "CLRO", "MOVO", "MOVAO", "PUSHAO",
NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,		/* 180 - 19F */
NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
"CVTFH", "CVTFG", NULL, NULL, NULL, NULL, NULL, NULL,
NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,		/* 1A0 - 1BF */
NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,		/* 1C0 - 1DF */
NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,		/* 1E0 - 1FF */
NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
NULL, NULL, NULL, NULL, NULL, NULL, "CVTHF", "CVTHD",
NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };

const char *altcod[] = {
"CLRF", "CLRD", "CLRG", "CLRH", "MOVAF", "MOVAD", "MOVAG", "MOVAH",
"PUSHAF", "PUSHAD", "PUSHAG", "PUSHAH", "BNEQU", "BEQLU", "BCC", "BCS",
NULL };

const int32 altop[] = {
 0xD4, 0x7C, 0x7C, 0x17C, 0xDE, 0x7E, 0x7E, 0x17E,
 0xDF, 0x7F, 0x7F, 0x17F, 0x12, 0x13, 0x1E, 0x1F };

const char* regname[] = {
 "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7",
 "R8", "R9", "R10", "R11", "AP", "FP", "SP", "PC" };

#define GETNUM(d,n)	for (k = d = 0; k < n; k++) \
			d = d | (((int32) val[vp++]) << (k * 8))

/* Symbolic decode

   Inputs:
	*of	=	output stream
	addr	=	current PC
	*val	=	values to decode
	*uptr	=	pointer to unit
	sw	=	switches
   Outputs:
	return	=	if >= 0, error code
			if < 0, number of extra bytes retired
*/

t_stat fprint_sym (FILE *of, t_addr exta, t_value *val,
	UNIT *uptr, int32 sw)
{
uint32 addr = (uint32) exta;
int32 c, k, num, vp, lnt, rdx;
t_stat r;
DEVICE *dptr;

if (uptr == NULL) uptr = &cpu_unit;			/* anon = CPU */
dptr = find_dev_from_unit (uptr);			/* find dev */
if (dptr == NULL) return SCPE_IERR;
if (dptr->dwidth != 8) return SCPE_ARG;			/* byte dev only */
if (sw & SWMASK ('B')) lnt = 1;				/* get length */
else if (sw & SWMASK ('W')) lnt = 2;
else if (sw & SWMASK ('L')) lnt = 4;
else lnt = (uptr == &cpu_unit)? 4: 1;
if (sw & SWMASK ('D')) rdx = 10;			/* get radix */
else if (sw & SWMASK ('O')) rdx = 8;
else if (sw & SWMASK ('H')) rdx = 16;
else rdx = dptr->dradix;
vp = 0;							/* init ptr */
if ((sw & SWMASK ('A')) || (sw & SWMASK ('C'))) {	/* char format? */
	if (sw & SWMASK ('C')) lnt = sim_emax;		/* -c -> string */
	if ((val[0] & 0x7F) == 0) return SCPE_ARG;
	while (vp < lnt) {				/* print string */
	    if ((c = (int32) val[vp++] & 0x7F) == 0) break;
	    fprintf (of, (c < 0x20)? "<%02X>": "%c", c);  }
	return -(vp - 1);  }				/* return # chars */

if (sw & SWMASK ('M') && (uptr == &cpu_unit)) {		/* inst format? */
	r = fprint_sym_m (of, addr, val);		/* decode inst */
	if (r <= 0) return r;  }

GETNUM (num, lnt);					/* get number */
fprint_val (of, (uint32) num, rdx, lnt * 8, PV_RZRO);
return -(vp - 1);
}

/* Symbolic decode for -m

   Inputs:
	of	=	output stream
	addr	=	current PC
	*val	=	values to decode
   Outputs:
	return	=	if >= 0, error code
			if < 0, number of extra bytes retired
*/

t_stat fprint_sym_m (FILE *of, uint32 addr, t_value *val)
{
int32 i, k, vp, inst, numspec;
int32 num, spec, rn, disp, index;

vp = 0;							/* init ptr */
inst = (int32) val[vp++];				/* get opcode */
if (inst == 0xFD) inst = 0x100 | (int32) val[vp++];	/* 2 byte op? */
if (opcode[inst] == NULL) return SCPE_ARG;		/* defined? */
numspec = drom[inst][0] & DR_NSPMASK;			/* get # spec */
if (numspec == 0) numspec = (drom[inst][0] & DR_USPMASK) >> 4;
fprintf (of, "%s", opcode[inst]);			/* print name */
for (i = 0; i < numspec; i++) {				/* loop thru spec */
	fputc (i? ',': ' ', of);			/* separator */
	disp = drom[inst][i + 1];			/* get drom value */
	if (disp == BB) {				/* byte br disp? */
	    GETNUM (num, 1);
	    fprintf (of, "%-X", SXTB (num) + addr + vp);  }
	else if (disp == BW) {				/* word br disp? */
	    GETNUM (num, 2);
	    fprintf (of, "%-X", SXTW (num) + addr + vp);  }
	else {
	    spec = (int32) val[vp++];			/* get specifier */
	    if ((spec & 0xF0) == IDX) {			/* index? */
		index = spec;				/* copy, get next */
		spec = (int32) val[vp++];  }
	    else index = 0;
	    rn = spec & 0xF;				/* get reg # */
	    switch (spec & 0xF0) {			/* case on mode */
	    case SH0: case SH1: case SH2: case SH3:	/* s^# */
		fprintf (of, "#%-X", spec);
		break;
	    case GRN:					/* Rn */
		fprintf (of, "%-s", regname[rn]);
		break;
	    case RGD:					/* (Rn) */
		fprintf (of, "(%-s)", regname[rn]);
		break;
	    case ADC:					/* -(Rn) */
		fprintf (of, "-(%-s)", regname[rn]);
		break;
	    case AIN:					/* (Rn)+, #n */
		if (rn != nPC) fprintf (of, "(%-s)+", regname[rn]);
		else {
		    if (disp == OC)
			vp = fprint_sym_qoimm (of, val, vp, 4);
		    else if (DR_LNT (disp) == L_QUAD)
			vp = fprint_sym_qoimm (of, val, vp, 2);
		    else {
		    	GETNUM (num, DR_LNT (disp));
			fprintf (of, "#%-X", num);  }  }
		break;
	    case AID:					/* @(Rn)+, @#n */
		if (rn != nPC) fprintf (of, "@(%-s)+", regname[rn]);
		else {
		    GETNUM (num, 4);
		    fprintf (of, "@#%-X", num);  }
		break;
	    case BDD:					/* @b^d(r),@b^n */
		fputc ('@', of);
	    case BDP:					/* b^d(r), b^n */
		GETNUM (num, 1);
		if (rn == nPC) fprintf (of, "%-X", addr + vp + SXTB (num));
		else if (num & BSIGN) fprintf (of, "-%-X(%-s)",
		    -num & BMASK, regname[rn]);
		else fprintf (of, "%-X(%-s)", num, regname[rn]);
		break;
	    case WDD:					/* @w^d(r),@w^n */
		fputc ('@', of);
	    case WDP:					/* w^d(r), w^n */
		GETNUM (num, 2);
		if (rn == nPC) fprintf (of, "%-X", addr + vp + SXTW (num));
		else if (num & WSIGN) fprintf (of, "-%-X(%-s)",
		    -num & WMASK, regname[rn]);
		else fprintf (of, "%-X(%-s)", num, regname[rn]);
		break;
	    case LDD:					/* @l^d(r),@l^n */
		fputc ('@', of);
	    case LDP:					/* l^d(r),l^n */
		GETNUM (num, 4);
		if (rn == nPC) fprintf (of, "%-X", addr + vp + num);
		else if (num & LSIGN) fprintf (of, "-%-X(%-s)",
		    -num, regname[rn]);
		else fprintf (of, "%-X(%-s)", num, regname[rn]);
		break;  }				/* end case */
	    if (index) fprintf (of, "[%-s]", regname[index & 0xF]);
	    }						/* end else */
	}						/* end for */
return -(vp - 1);
}

/* Symbolic decode, quad/octa immediates

   Inputs:
	*of	=	output stream
	*val	=	pointer to input values
	vp	=	current index into val
	lnt	=	number of longwords in immediate
   Outputs:
	vp	=	updated index into val
*/

int32 fprint_sym_qoimm (FILE *of, t_value *val, int32 vp, int32 lnt)
{
int32 i, k, startp, num[4];

for (i = 0; i < lnt; i++) { GETNUM (num[lnt - 1 - i], 4); }
for (i = startp = 0; i < lnt; i++) {
	if (startp) fprintf (of, "%08X", num[i]);
	else if (num[i] || (i == (lnt - 1))) {
	    fprintf (of, "#%-X", num[i]);
	    startp = 1;  }  }
return vp;
}

#define PUTNUM(d,n)	for (k = 0; k < n; k++) val[vp++] = (d >> (k * 8)) & 0xFF

/* Symbolic input

   Inputs:
	*cptr	=	pointer to input string
	addr	=	current PC
	*uptr	=	pointer to unit
	*val	=	pointer to output values
	sw	=	switches
   Outputs:
	status	=	> 0   error code
			<= 0  -number of extra words
*/

t_stat parse_sym (char *cptr, t_addr exta, UNIT *uptr, t_value *val, int32 sw)
{
uint32 addr = (uint32) exta;
int32 k, rdx, lnt, num, vp;
t_stat r;
DEVICE *dptr;
static const uint32 maxv[5] = { 0, 0xFF, 0xFFFF, 0, 0xFFFFFFFF };

if (uptr == NULL) uptr = &cpu_unit;			/* anon = CPU */
dptr = find_dev_from_unit (uptr);			/* find dev */
if (dptr == NULL) return SCPE_IERR;
if (dptr->dwidth != 8) return SCPE_ARG;			/* byte dev only */
if (sw & SWMASK ('B')) lnt = 1;				/* get length */
else if (sw & SWMASK ('W')) lnt = 2;
else if (sw & SWMASK ('L')) lnt = 4;
else lnt = (uptr == &cpu_unit)? 4: 1;
if (sw & SWMASK ('D')) rdx = 10;			/* get radix */
else if (sw & SWMASK ('O')) rdx = 8;
else if (sw & SWMASK ('H')) rdx = 16;
else rdx = dptr->dradix;
vp = 0;
if ((sw & SWMASK ('A')) || (sw & SWMASK ('C'))) {	/* char format? */
	if (sw & SWMASK ('C')) lnt = sim_emax;		/* -c -> string */
	if (*cptr == 0) return SCPE_ARG;
	while ((vp < lnt) && *cptr) {			/* get chars */
	    val[vp++] = *cptr++;  }
	return -(vp - 1);  }				/* return # chars */

if (uptr == &cpu_unit) {				/* cpu only */
	r = parse_sym_m (cptr, addr, val);		/* try to parse inst */
	if (r <= 0) return r;  }

num = (int32) get_uint (cptr, rdx, maxv[lnt], &r);	/* get number */
if (r != SCPE_OK) return r;
PUTNUM (num, lnt);					/* store */
return -(lnt - 1);
}

/* Symbolic input for -m

   Inputs:
	*cptr	=	pointer to input string
	addr	=	current PC
	*val	=	pointer to output values
   Outputs:
	status	=	> 0   error code
			<= 0  -number of extra words
*/

t_stat parse_sym_m (char *cptr, uint32 addr, t_value *val)
{
int32 i, numspec, disp, opc, vp;
t_stat r;
char gbuf[CBUFSIZE];

cptr = get_glyph (cptr, gbuf, 0);			/* get opcode */
for (i = 0, opc = -1; (i < NUM_INST) && (opc < 0); i++) {
	if (opcode[i] && strcmp (gbuf, opcode[i]) == 0) opc = i;  }
if (opc < 0) {						/* check alternates */
	for (i = 0; altcod[i] && (opc < 0); i++) {
	    if (strcmp (gbuf, altcod[i]) == 0) opc = altop[i];  }  }
if (opc < 0) return SCPE_ARG;				/* undefined? */
vp = 0;
if (opc >= 0x100) val[vp++] = 0xFD;			/* 2 byte? */
val[vp++] = opc & 0xFF;					/* store opcode */
numspec = drom[opc][0] & DR_NSPMASK;			/* get # specifiers */
if (numspec == 0) numspec = (drom[opc][0] & DR_USPMASK) >> 4;
for (i = 1; i <= numspec; i++) {			/* loop thru specs */
	if (i == numspec) cptr = get_glyph (cptr, gbuf, 0);
	else cptr = get_glyph (cptr, gbuf, ',');	/* get specifier */
	disp = drom[opc][i];				/* get drom value */
	if (disp == BB) vp = parse_brdisp (gbuf, addr, val, vp, 0, &r);
	else if (disp == BW) vp = parse_brdisp (gbuf, addr, val, vp, 1, &r);
	else vp = parse_spec (gbuf, addr, val, vp, disp, &r);
	if (r != SCPE_OK) return r;  }
if (*cptr != 0) return SCPE_ARG;
return -(vp - 1);
}

/* Parse a branch displacement

   Inputs:
	cptr	=	pointer to input buffer
	addr	=	current address
	val	=	pointer to output array
	vp	=	current pointer in output array
	lnt	=	length (0 = byte, 1 = word)
	r	=	pointer to status
   Outputs:
	vp	=	updated output pointer
*/

int32 parse_brdisp (char *cptr, uint32 addr, t_value *val, int32 vp,
	int32 lnt, t_stat *r)
{
int32 k, dest, num;

dest = (int32) get_uint (cptr, 16, 0xFFFFFFFF, r);	/* get value */
num = dest - (addr + vp + lnt + 1);			/* compute offset */
if ((num > (lnt? 32767: 127)) || (num < (lnt? -32768: -128))) return SCPE_ARG;
PUTNUM (num, lnt + 1);					/* store offset */
return vp;
}

/* Parse a specifier

   Inputs:
	cptr	=	pointer to input buffer
	addr	=	current address
	val	=	pointer to output array
	vp	=	current pointer in output array
	disp	=	specifier dispatch
	r	=	pointer to status
   Outputs:
	vp	=	updated output pointer
*/

#define SP_IND		0x200				/* indirect */
#define SP_V_FORCE	6
#define  SP_FS		0x040				/* S^ */
#define  SP_FI		0x080				/* I^ */
#define  SP_FB		0x0C0				/* B^ */
#define  SP_FW		0x100				/* W^ */
#define  SP_FL		0x140				/* L^ */
#define SP_LIT		0x020				/* # */
#define SP_PLUS		0x010				/* plus */
#define SP_MINUS	0x008				/* minus */
#define SP_NUM		0x004				/* number */
#define SP_IDX		0x002				/* (Rn) */
#define SP_POSTP	0x001				/* trailing + */
#define M1C(c,v)	if (*cptr == c) { cptr++; fl = fl | v; }
#define SPUTNUM(v,d)	if (fl & SP_MINUS) v = -v; PUTNUM (v, d)
#define PARSE_LOSE	{ *r = SCPE_ARG; return vp; }
#define SEL_LIM(p,m,u)	((fl & SP_PLUS)? (p): ((fl & SP_MINUS)? (m): (u)))

int32 parse_spec (char *cptr, uint32 addr, t_value *val, int32 vp, int32 disp, t_stat *r)
{
int32 i, k, litsize, rn, index;
int32 num, dispsize, mode;
int32 lit[4] = { 0 };
int32 fl = 0;
char c, *tptr;
const char *force[] = { "S^", "I^", "B^", "W^", "L^", NULL };

*r = SCPE_OK;						/* assume ok */
M1C ('@', SP_IND);					/* look for @ */
if (tptr = parse_rnum (cptr, &rn)) {			/* look for Rn */
	if (*cptr == '[') {				/* look for [Rx] */
	    cptr = parse_rnum (++cptr, &index);
	    if ((cptr == NULL) || (*cptr++ != ']')) PARSE_LOSE;
	    val[vp++] = index | IDX;  }
	else val[vp++] = rn | GRN | (fl? 1: 0);		/* Rn or @Rn */
	if (*tptr != 0) *r = SCPE_ARG;			/* must be done */
	return vp;  }
for (i = 0; force[i]; i++) {				/* look for x^ */
	if (strncmp (cptr, force[i], 2) == 0) {
	    cptr = cptr + 2;
	    fl = fl | ((i + 1) << SP_V_FORCE);
	    break;  }  }
M1C ('#', SP_LIT);					/* look for # */
M1C ('+', SP_PLUS);					/* look for + */
M1C ('-', SP_MINUS);					/* look for - */
for (litsize = 0;; cptr++) {				/* look for mprec int */
	c = *cptr;
	if ((c < '0') || (c > 'F') || ((c > '9') && (c < 'A'))) break;
	num = (c <= '9')? c - '0': c - 'A' + 10;
	fl = fl | SP_NUM;
	for (i = 3; i >= 0; i--) {
	    lit[i] = lit[i] << 4;
	    if (i > 0) lit[i] = lit[i] | ((lit[i - 1] >> 28) & 0xF);
	    else lit[i] = lit[i] | num;
	    if (lit[i] && (i > litsize)) litsize = i;  }  }
if (*cptr == '(') {					/* look for (Rn) */
	cptr = parse_rnum (++cptr, &rn);
	if ((cptr == NULL) || (*cptr++ != ')')) PARSE_LOSE;
	fl = fl | SP_IDX;  }
M1C ('+', SP_POSTP);					/* look for + */
if (*cptr == '[') {					/* look for [Rx] */	
	cptr = parse_rnum (++cptr, &index);
	if ((cptr == NULL) || (*cptr++ != ']')) PARSE_LOSE;
	val[vp++] = index | IDX;  }
switch (fl) {						/* case on state */
case SP_FS|SP_LIT|SP_NUM:				/* S^#n */
case SP_FS|SP_LIT|SP_PLUS|SP_NUM:			/* S^#+n */
	if ((litsize > 0) || (lit[0] & ~0x3F)) PARSE_LOSE;
	val[vp++] = lit[0];
	break;
case SP_IDX:						/* (Rn) */
	val[vp++] = rn | RGD;
	break;
case SP_MINUS|SP_IDX:					/* -(Rn) */
	val[vp++] = rn | ADC;
	break;
case SP_IDX|SP_POSTP:					/* (Rn)+ */
	val[vp++] = rn | AIN;
	break;
case SP_LIT|SP_NUM:					/* #n */
case SP_LIT|SP_PLUS|SP_NUM:				/* #+n */
	if ((litsize == 0) && ((lit[0] & ~0x3F) == 0)) {
	    val[vp++] = lit[0];
	    break;  }
case SP_LIT|SP_MINUS|SP_NUM:				/* #-n */
case SP_FI|SP_LIT|SP_NUM:				/* I^#n */
case SP_FI|SP_LIT|SP_PLUS|SP_NUM:			/* I^#+n */
case SP_FI|SP_LIT|SP_MINUS|SP_NUM:			/* I^#-n */
	val[vp++] = nPC | AIN;
	disp = (disp == OC)? DR_LNMASK + 1: disp & DR_LNMASK;
	switch (disp) {					/* case spec lnt */
	case 00:					/* check fit */
	    if ((litsize > 0) || (lit[0] < 0) || 
		(lit[0] > SEL_LIM (0x7F, 0x80, 0xFF))) PARSE_LOSE;
	    SPUTNUM (lit[0], 1);			/* store */
	    break;
	case 01:					/* check fit */
	    if ((litsize > 0) || (lit[0] < 0) ||
		(lit[0] > SEL_LIM (0x7FFF, 0x8000, 0xFFFF))) PARSE_LOSE;
	    SPUTNUM (lit[0], 2);
	    break;
	case 02:					/* check 1 lw */
	    if (litsize > 0) PARSE_LOSE;
	    SPUTNUM (lit[0], 4);
	    break;
	case 03:					/* check 2 lw */
	    if (litsize > 1) PARSE_LOSE;
	    vp = parse_sym_qoimm (lit, val, vp, 2, fl & SP_MINUS);
	    break;
	case DR_LNMASK + 1:
	    vp = parse_sym_qoimm (lit, val, vp, 4, fl & SP_MINUS);
	    break;  }				/* end case disp */
	break;
case SP_IND|SP_IDX|SP_POSTP:				/* @(Rn)+ */
	val[vp++] = rn | AID;
	break;
case SP_IND|SP_LIT|SP_NUM:				/* @#n */
	if (litsize > 0) PARSE_LOSE;
	val[vp++] = nPC | AID;
	PUTNUM (lit[0], 4);
	break;
case SP_NUM|SP_IDX:					/* d(rn) */
case SP_PLUS|SP_NUM|SP_IDX:				/* +d(rn) */
case SP_MINUS|SP_NUM|SP_IDX:				/* -d(rn) */
case SP_IND|SP_NUM|SP_IDX:				/* @d(rn) */
case SP_IND|SP_PLUS|SP_NUM|SP_IDX:			/* @+d(rn) */
case SP_IND|SP_MINUS|SP_NUM|SP_IDX:			/* @-d(rn) */
	if (litsize > 0) PARSE_LOSE;
	dispsize = 4;					/* find fit for */
	mode = LDP;					/* displacement */
	if (lit[0] >= 0) {
	    if (lit[0] <= SEL_LIM (0x7F, 0x80, 0xFF)) {
		dispsize = 1;
		mode = BDP;  }
	    else if (lit[0] <= SEL_LIM (0x7FFF, 0x8000, 0xFFFF)) {
		dispsize = 2;
		mode = WDP;  }  }
	val[vp++] = mode | rn | ((fl & SP_IND)? 1: 0);
	SPUTNUM (lit[0], dispsize);
	break;
case SP_FB|SP_NUM|SP_IDX:				/* B^d(rn) */
case SP_FB|SP_PLUS|SP_NUM|SP_IDX:			/* B^+d(rn) */
case SP_FB|SP_MINUS|SP_NUM|SP_IDX:			/* B^-d(rn) */
case SP_IND|SP_FB|SP_NUM|SP_IDX:			/* @B^d(rn) */
case SP_IND|SP_FB|SP_PLUS|SP_NUM|SP_IDX:		/* @B^+d(rn) */
case SP_IND|SP_FB|SP_MINUS|SP_NUM|SP_IDX:		/* @B^-d(rn) */
	if ((litsize > 0) || (lit[0] < 0) || 
	    (lit[0] > SEL_LIM (0x7F, 0x80, 0xFF))) PARSE_LOSE;
	val[vp++] = rn | BDP | ((fl & SP_IND)? 1: 0);
	SPUTNUM (lit[0], 1);
	break;
case SP_FW|SP_NUM|SP_IDX:				/* W^d(rn) */
case SP_FW|SP_PLUS|SP_NUM|SP_IDX:			/* W^+d(rn) */
case SP_FW|SP_MINUS|SP_NUM|SP_IDX:			/* W^-d(rn) */
case SP_IND|SP_FW|SP_NUM|SP_IDX:			/* @W^d(rn) */
case SP_IND|SP_FW|SP_PLUS|SP_NUM|SP_IDX:		/* @W^+d(rn) */
case SP_IND|SP_FW|SP_MINUS|SP_NUM|SP_IDX:		/* @W^-d(rn) */
	if ((litsize > 0) || (lit[0] < 0) ||
	    (lit[0] > SEL_LIM (0x7FFF, 0x8000, 0xFFFF))) PARSE_LOSE;
	val[vp++] = rn | WDP | ((fl & SP_IND)? 1: 0);
	SPUTNUM (lit[0], 2);
	break;
case SP_FL|SP_NUM|SP_IDX:				/* L^d(rn) */
case SP_FL|SP_PLUS|SP_NUM|SP_IDX:			/* L^+d(rn) */
case SP_FL|SP_MINUS|SP_NUM|SP_IDX:			/* L^-d(rn) */
case SP_IND|SP_FL|SP_NUM|SP_IDX:			/* @L^d(rn) */
case SP_IND|SP_FL|SP_PLUS|SP_NUM|SP_IDX:		/* @L^+d(rn) */
case SP_IND|SP_FL|SP_MINUS|SP_NUM|SP_IDX:		/* @L^-d(rn) */
	if ((litsize > 0) || (lit[0] < 0)) PARSE_LOSE;
	val[vp++] = rn | LDP | ((fl & SP_IND)? 1: 0);
	SPUTNUM (lit[0], 2);
	break;
case SP_NUM:						/* n */
case SP_IND|SP_NUM:					/* @n */
	if (litsize > 0) PARSE_LOSE;
	num = lit[0] - (addr + vp + 2);			/* fit in byte? */
	if ((num >= -128) && (num <= 127)) {
	    mode = BDP;
	    dispsize = 1;  }
	else {
	    num = lit[0] - (addr + vp + 3);		/* fit in word? */
	    if ((num >= -32768) && (num <= 32767)) {
		mode = WDP;
		dispsize = 2;  }
	    else {
	    	num = lit[0] - (addr + vp + 5);		/* no, use lw */
		mode = LDP;
		dispsize = 4;  }  }
	val[vp++] = mode | nPC | ((fl & SP_IND)? 1: 0);
	PUTNUM (num, dispsize);
	break;
case SP_FB|SP_NUM:					/* B^n */
case SP_IND|SP_FB|SP_NUM:				/* @B^n */
	num = lit[0] - (addr + vp + 2);
	if ((litsize > 0) || (num > 127) || (num < -128)) PARSE_LOSE;
	val[vp++] = nPC | BDP | ((fl & SP_IND)? 1: 0);
	PUTNUM (num, 1);
	break;
case SP_FW|SP_NUM:					/* W^n */
case SP_IND|SP_FW|SP_NUM:				/* @W^n */
	num = lit[0] - (addr + vp + 3);
	if ((litsize > 0) || (num > 32767) || (num < -32768)) PARSE_LOSE;
	val[vp++] = nPC | WDP | ((fl & SP_IND)? 1: 0);
	PUTNUM (num, 2);
	break;
case SP_FL|SP_NUM:					/* L^n */
case SP_IND|SP_FL|SP_NUM:				/* @L^n */
	num = lit[0] - (addr + vp + 5);
	if (litsize > 0) PARSE_LOSE;
	val[vp++] = nPC | LDP | ((fl & SP_IND)? 1: 0);
	PUTNUM (num, 4);
	break;
default:
	PARSE_LOSE;  }					/* end case */
if (*cptr != 0) *r = SCPE_ARG;				/* must be done */
return vp;
}

char *parse_rnum (char *cptr, int32 *rn)
{
int32 i, lnt;
t_value regnum;
char *tptr;

for (i = 15; i >= 0; i--) {				/* chk named reg */
	lnt = strlen (regname[i]);
	if (strncmp (cptr, regname[i], lnt) == 0) {
	    *rn = i;
	    return cptr + lnt;  }  }
if (*cptr++ != 'R') return NULL;			/* look for R */
regnum = strtotv (cptr, &tptr, 10);			/* look for reg # */
if ((cptr == tptr) || (regnum > 15)) return NULL;
*rn = (int32) regnum;
return tptr;
}

int32 parse_sym_qoimm (int32 *lit, t_value *val, int32 vp, int lnt, int32 minus)
{
int32 i, k, prev;

for (i = prev = 0; i < lnt; i++) {
	if (minus) prev = lit[i] = ~lit[i] + (prev == 0);
	PUTNUM (lit[i], 4);  }
return vp;
}

