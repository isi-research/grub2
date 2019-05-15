/* hello.c - test module for dynamic loading */
/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2003,2007  Free Software Foundation, Inc.
 *  Copyright (C) 2003  NIIBE Yutaka <gniibe@m17n.org>
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  GRUB is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Copyright 2017-2019, Intuitive Surgical Operations, Inc. All rights reserved.
 *
 * This source code is licensed under the GPLv3 license found in
 * the COPYING file in the root directory of this source tree.
 */

#include <grub/types.h>
#include <grub/misc.h>
#include <grub/mm.h>
#include <grub/err.h>
#include <grub/dl.h>
#include <grub/env.h>
#include <grub/extcmd.h>
#include <grub/i18n.h>

#define REPLY_BUFFER_SIZE   16

GRUB_MOD_LICENSE ("GPLv3+");

typedef enum _isi_math_operation
{
    ISI_MATH_OP_NONE = 0,
    ISI_MATH_OP_INC,
    ISI_MATH_OP_DEC, 
    ISI_MATH_OP_COUNT
} isi_math_op_t;

static const struct grub_arg_option options[] =
{
    {"set", 's', 0, N_("set"), N_("SET"), ARG_TYPE_STRING},
    {0, 0, 0, 0, 0, 0}
};

static grub_err_t grub_cmd_isi_math(grub_extcmd_context_t ctxt, int argc, char **args )
{
    int set_env_var = 0;
    struct grub_arg_list *state = ctxt->state;
    isi_math_op_t math_op = ISI_MATH_OP_NONE;
    long input_arg;
    char reply[REPLY_BUFFER_SIZE];

    if( state[0].set )
    {
        set_env_var = 1;
    }

    if( 0 == grub_strcmp(ctxt->extcmd->cmd->name, "inc") )
    {
        math_op = ISI_MATH_OP_INC;

    }
    else if( 0 ==  grub_strcmp(ctxt->extcmd->cmd->name, "dec") )
    {
        math_op = ISI_MATH_OP_DEC;
    }
    else
    {
        return grub_error(GRUB_ERR_BUG, N_("BUG: DID NOT RECOGNIZE COMMAND NAME"));
    }

    // validate the arg count
    switch(math_op)
    {
        case ISI_MATH_OP_INC:
        case ISI_MATH_OP_DEC:
            if (1 != argc)
            {
                return grub_error(GRUB_ERR_BAD_ARGUMENT, N_("EXPECTED A SINGLE ARG"));
            }
            break;

        default:
            return grub_error(GRUB_ERR_BUG, N_("BUG: INVALID ISI MATH OP TYPE"));

    }

    input_arg = grub_strtol(args[0], NULL, 10);

    switch(math_op)
    {
        case ISI_MATH_OP_INC:
            input_arg++;
            break;
        case ISI_MATH_OP_DEC:
            input_arg--;
            break;
        default:
            return grub_error(GRUB_ERR_BUG, N_("BUG: INVALID ISI MATH OP TYPE"));
    }

    if( set_env_var )
    {
        // set the env var w/ the new value
        grub_snprintf( reply, REPLY_BUFFER_SIZE, "%ld", input_arg );
        grub_env_set( state[0].arg, reply );
    }
    else
    {
        grub_printf("%ld\n", input_arg);
    }

    return GRUB_ERR_NONE;
}

static grub_extcmd_t cmd_inc;
static grub_extcmd_t cmd_dec;

GRUB_MOD_INIT(hello)
{
    cmd_inc = grub_register_extcmd ("inc", grub_cmd_isi_math, 0, 0,
			      N_("increment the input number"), options);

    cmd_dec = grub_register_extcmd ("dec", grub_cmd_isi_math, 0, 0,
			      N_("decrement the input number"), options);
}

GRUB_MOD_FINI(hello)
{
    grub_unregister_extcmd (cmd_inc);
    grub_unregister_extcmd (cmd_dec);
}
