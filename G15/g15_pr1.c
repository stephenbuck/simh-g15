#include "g15_defs.h"

t_stat g15_pr1_rd(int32 *data, int32 PA, int32 access);
t_stat g15_pr1_wr(int32 data, int32 PA, int32 access);
t_stat g15_pr1_reset(DEVICE *dptr);
t_stat g15_pr1_attach(UNIT *uptr, CONST char *ptr);
t_stat g15_pr1_detach(UNIT *uptr);
t_stat g15_pr1_help(FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag, const char *cptr);
const char *g15_pr1_desc(DEVICE *dptr);

UNIT g15_pr1_unit =
{
    next:            NULL,
    action:          &g15_pr1_svc,
    filename:        NULL,
    fileref:         NULL,
    filebuf:         NULL,
    hwmark:          0,
    time:            0,
    flags:           UNIT_ATTABLE | UNIT_RO | UNIT_SEQ | UNIT_DISABLE,
    dynflags:        0,
    capac:           0,
    pos:             0,
    io_flush:        NULL,
    iostarttime:     0,
    buf:             0,
    wait:            0,
    u3:              0,
    u4:              0,
    u5:              0,
    u6:              0,
    up7:             NULL,
    up8:             NULL,
    us9:             0,
    us10:            0,
    disk_type:       0,
    tmxr:            NULL,
    recsize:         0,
    tape_eom:        0,
    cancel:          NULL,
    usecs_remaining: 0,
    uname:           NULL,
    dptr:            NULL,
    dctrl:           0
};

REG g15_pr1_reg[] =
{
};

MTAB g15_pr1_mod[] =
{
};

DEVICE g15_pr1_dev =
{
    name:        "PR-1",
    units:       &g15_pr1_unit,
    registers:   g15_pr1_reg,
    modifiers:   g15_pr1_mod,
    numunits:    1,
    aradix:      10,
    awidth:      31,
    aincr:       1,
//    dradix:      DEV_RDX,
    dwidth:      8,
    examine:     NULL,
    deposit:     NULL,
    reset:       &g15_pr1_reset,
    boot:        NULL,
    attach:      &g15_pr1_attach,
    detach:      &g15_pr1_detach,
    ctxt:        NULL,
    flags:       DEV_DISABLE,
    dctrl:       0,
    debflags:    NULL,
//    memsize:     NULL,
    lname:       NULL,
    help:        &g15_pr1_help,
    attach_help: NULL,
    help_ctx:    NULL,
    description: &g15_pr1_desc,
    brk_types:   NULL,
    type_ctx:    NULL
};

t_stat g15_pr1_svc(UNIT *uptr)
{
   if ((uptr->flags & UNIT_ATT) == 0)
        return SCPE_UNATT;
#if 0
int32   temp;
if ((temp = getc (pr1_unit.fileref)) == EOF) {
    if (feof (pr1_unit.fileref)) {
        if (pr1_stopioe)
            sim_printf ("PTR end of file\n");
        else return SCPE_OK;
        }
    else sim_perror ("PTR I/O error");
    clearerr (pr1_unit.fileref);
    return SCPE_IOERR;
    }
pr1_unit.buf = temp & ((pr1_unit.flags & UNIT_8B)? 0377: 0177);
#endif
    g15_pr1_unit.pos += 1;
    return SCPE_OK;
}

t_stat g15_pr1_reset(DEVICE *dptr)
{
    g15_pr1_unit.buf = 0;
    sim_cancel(&g15_pr1_unit);
    return SCPE_OK;
}

t_stat g15_pr1_attach(UNIT *uptr, CONST char *cptr)
{
    return attach_unit(uptr, cptr);
}

t_stat g15_pr1_detach(UNIT *uptr)
{
    return detach_unit(uptr);
}

t_stat g15_pr1_help(FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag, const char *cptr)
{
    return SCPE_OK;
}

const char *g15_pr1_desc(DEVICE *dptr)
{
    return "PR-1 Paper Tape Reader";
}

t_stat g15_pr1_cmd(uint16_t cmd)
{
    g15_util_trace_enter(__FUNCTION__);
    switch (cmd)
    {
        case G15_PR1_CMD_READ:
            break;
        case G15_PR1_CMD_REVERSE:
            break;
        default:
            break;
    }
    g15_util_trace_leave();
    return SCPE_OK;
}