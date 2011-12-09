/**************************************************************************
 *
 * Copyright 2010 VMware, Inc.
 * Copyright 2009 VMware, Inc.
 * Copyright 2007-2008 Tungsten Graphics, Inc., Cedar Park, Texas.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#include "gallivm/lp_bld_tgsi.h"
#include "tgsi/tgsi_parse.h"
#include "util/u_memory.h"

/* The user is responsible for freeing list->instructions */
unsigned lp_bld_tgsi_list_init(struct lp_build_tgsi_inst_list * list)
{
   list->instructions = (struct tgsi_full_instruction *)
         MALLOC( LP_MAX_INSTRUCTIONS * sizeof(struct tgsi_full_instruction) );
   if (!list->instructions) {
      return 0;
   }
   list->max_instructions = LP_MAX_INSTRUCTIONS;
   return 1;
}


unsigned lp_bld_tgsi_add_instruction(
   struct lp_build_tgsi_inst_list * list,
   struct tgsi_full_instruction *inst_to_add)
{

   if (list->num_instructions == list->max_instructions) {
      struct tgsi_full_instruction *instructions;
      instructions = REALLOC(list->instructions, list->max_instructions
                                      * sizeof(struct tgsi_full_instruction),
                                      (list->max_instructions + LP_MAX_INSTRUCTIONS)
                                      * sizeof(struct tgsi_full_instruction));
      if (!instructions) {
         return 0;
      }
      list->instructions = instructions;
      list->max_instructions += LP_MAX_INSTRUCTIONS;
   }
   memcpy(list->instructions + list->num_instructions, inst_to_add,
          sizeof(list->instructions[0]));

   list->num_instructions++;

   return 1;
}

