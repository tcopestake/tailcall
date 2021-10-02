/***
 *
 * For the rundown on how this all works, check ../README.md
 *
 **/

#include <stddef.h>
#include <stdbool.h>
#include "php.h"
#include "zend_extensions.h"
#include "tailcall.h"

/*
 * Creates a new optomisation context.
 *
 * (In practice, the context is just a container for all relevant data.)
 */
tco_context *tco_new_context(zend_op_array *op_array)
{
    tco_call_info *call_info;

    tco_context *context = malloc(sizeof(tco_context));

    context->do_optimise = false;
    context->op_array = op_array;
    context->t_remaps = NULL;
    context->start_address = 0;

    // This is an offset to where the original opcodes end - and the beginning
    // of where our new opcodes will be inserted.

    context->appendix_offset = op_array->last;

    /*
     * When/if we start to rewrite the opcodes, we'll need to know how much
     * memory to allocate to store the original & our new opcodes. This value
     * will be used to keep track of that.
     * (It is modified accordingly elsewhere.)
     */

    context->required_opcode_count = op_array->last;

    // Allocate enough memory for the call info pool & point to the tail.

    call_info = context->call_pool_tail = malloc(sizeof(tco_resursive_call) * TCO_CALL_POOL_SIZE);

    // We need to set some initial values for the first call info structure.

    call_info->number = 1;
    call_info->previous = NULL;
    call_info->pseudo_ops = NULL;

    // (Have a guess what this does.)

    return context;
}

/*
 * Returns a new tco_call_info structure within the current context.
 *
 * If a structure is free within the pool, it will be used.
 * Otherwise, a new one will be dynamically allocated on-the-fly.
 */
void *tco_get_new_call_info(tco_context *context)
{
    tco_call_info *current_info;
    tco_call_info *new_info;

    current_info = context->call_info_tail;

    // If current_info->number is 1, current_info is still free to be used.

    if (current_info->number == 1) {
        new_info = current_info;
    } else {
        // Here we can either use the next free structure or allocate a new one.

        if (current_info->number < TCO_CALL_POOL_SIZE) {
           new_info = context->call_info_tail + current_info->number;
        } else {
            // (Maybe allocating a new pool would be better?)

            new_info = malloc(sizeof(tco_call_info));
        }

        // Set the number of this new structure.

        new_info->number = current_info->number + 1;

        // Map various pointers.

        new_info->previous = current_info;

        context->op_block_tail = new_info;
    }

    // Either way, initial values need to be set, etc.

    new_info->next_pseudo_op = 0;

    // Allocate new memory for the new opcodes.

    new_info->pseudo_ops = emalloc(sizeof(zend_op) * context->op_array->num_args);

    // (Some additional processing will go here eventually.)

    // Return t'structure.

    return new_info;
}

/*
 * Frees all memory associated with a given context - including memory
 * allocated for the context itself.
 */
void tco_free_context(tco_context *context)
{
    // Free any additional memory allocated for call info outside of the pool.

    tco_call_info *call_info;
    tco_call_info *next_info = context->call_pool_tail;

    while (next_info) {
        call_info = next_info;

        // Free any memory allocated for additional opcodes.

        if (call_info->pseudo_ops) {
            efree(call_info->pseudo_ops);
        }

        // Point to the next (technically previous) structure.

        next_info = call_info->previous;

        // If this structure was dynamically allocated, free it.

        if (call_info->number < TCO_CALL_POOL_SIZE) {
            free(call_info);
        }
    }

    // Free the memory allocated for the pool.

    free(context->call_pool_tail);

    // Free any memory allocated for T var remaps.

    if (context->t_remaps) {
        free(context->t_remaps);
    }

    // (We need to free any Zend strings/vars here somewhere eventually.)

    // Free the memory allocated for the context itself.

    free(context);
}

/*
 * Converts a given opcode to a ZEND_NOP.
 */
inline void tco_nop_out(zend_op *op)
{
	op->opcode = ZEND_NOP;

	SET_UNUSED(op->op1);
	SET_UNUSED(op->op2);
	SET_UNUSED(op->result);
}

/*
 * Converts a given opcode to a ZEND_JMP to a given address.
 */
inline void tco_make_jmp(zend_op *op, uint32_t address)
{
	op->opcode = ZEND_JMP;
	op->extended_value = 0;
	op->op1.opline_num = address;

	SET_UNUSED(op->op1);
	SET_UNUSED(op->op2);
	SET_UNUSED(op->result);
}

/*
 * Updates the given context with rewritten opcodes.
 */
void tco_rewrite_opcodes(tco_context *context)
{
    zend_op *op;

    tco_call_info *call_info;

    uint32_t destination_index;

    zend_op_array *op_array = context->op_array;

    // Allocate new memoir for the assembled opcodes.
    // (The required size should be tracked in context->required_opcode_count.)

    zend_op *new_ops = emalloc(sizeof(zend_op) * context->required_opcode_count);

    // memset(new_ops, 0x00, sizeof(zend_op) * context->required_opcode_count);

    // Copy all original opcodes to the new array.

    memcpy(
        new_ops,
        op_array->opcodes,
        sizeof(zend_op) * op_array->last
    );

    // Now we need to copy the opcodes created for each recursive call.
    // (If we've got this far, there should be at least 1 call w/ opcodes.)

    destination_index = op_array->last;

    call_info = context->call_info_tail;

    while (call_info) {
        // Copy the opcodes...

        memcpy(
            new_ops + destination_index,
            call_info->pseudo_ops,
            sizeof(zend_op) * op_array->num_args
        );

        // Update the destination_index (+1 to account for the jump)...

        destination_index += op_array->num_args + 1;

        // Create a jump back to the start of the function.

        tco_make_jmp(&new_ops[destination_index - 1], context->start_address);

        // Point call_info to the next (technically previous) structure.

        call_info = call_info->previous;
    }

    // Free the memory for the old opcodes; it's not needed anymore.
    // & then point the op array to the new opcodes.

	efree(op_array->opcodes);

    op_array->opcodes = new_ops;

    // Update op_array->last to account for the new opcodes.

    op_array->last = context->required_opcode_count;
}

/*
 * Determines whether a given init opcode is a recursive function call.
 */
bool tco_is_call_recursive(zend_op_array *op_array, zend_op *op)
{
    // Make sure operand 2 is a constant.

    if (op->op2_type != IS_CONST) {
        return false;
    }

    // For methods, we also need to check that the class/"scope" matches.

    switch (op->opcode) {
        case ZEND_INIT_STATIC_METHOD_CALL:
            // (We may not need to verify op_array->scope here, but I will just in caaase.)

            if (!op_array->scope || !op_array->scope->name) {
                return false;
            }

            // If operand 1 is a constant/string, compare it to the current class/scope name.

            if (op->op1_type == IS_CONST) {
                if (
                    !zend_string_equals(
                        Z_STR_P(CT_CONSTANT_EX(op_array, op->op1.constant)),
                        op_array->scope->name
                    )
                ) {
                    return false;
                }
            } else if (op->op1_type != IS_UNUSED) {
                // I'm not 100% on this, but I think this will cover both self:: and static::
                // If we're here, the call isn't recursive.

                return false;
            }

            break;

        case ZEND_INIT_METHOD_CALL:
            // I'm pretty sure operand 1 being "unused" means $this, but I could be wrong.

            if (op->op1_type != IS_UNUSED) {
                // operand 1 isn't $this; can't be recursive.

                return false;
            }

            break;
    }

    /*
     * If we got this far, the call either isn't a method call - or the
     * method call was within the same class/scope/whatever.
     * In either case, we now need to check the callable name.
     */

    return zend_string_equals(
        Z_STR_P(CT_CONSTANT_EX(op_array, op->op2.constant)),
        op_array->function_name
    );
}

/*
 * Returns an array for tracking T var remaps.
 */
tco_t_remap *tco_get_t_remaps(tco_context *context)
{
    // Each existing T variable will potentially need its own remap.

    size_t bytes_required = sizeof(tco_t_remap) * context->op_array->T;

    // If remaps haven't already been allocated, we need to allocate 'em.

    if (!context->t_remaps) {
        context->t_remaps = (tco_t_remap *) malloc(bytes_required);

        // While we're here, we should probably update T to reflect the new (expected) number.
        // (This may make more sense done elsewhere, but it's here for now at least.)

        context->op_array->T += context->op_array->T;
    }

    // (We also need to intialise everything to zero.)

    memset(context->t_remaps, 0x00, bytes_required);

    return context->t_remaps;
}

/*
 * Remaps the T variable used by a given operand - if applicable.
 */
inline void tco_do_operand_remaps(zend_uchar type, znode_op operand, tco_t_remap *t_remaps)
{
    if (type == IS_TMP_VAR) {
        // If the given T var has been remapped, we need to remap this operand.

        uint32_t remap_to = t_remaps[operand.var];

        if (remap_to > 0) {
            operand.var = remap_to;
        }
    }
}

/*
 * Finds the index (0-based) of a given argument, by name.
 * In theory, there should never be any situation in which this would
 * fail to find a value.
 */
uint32_t tco_find_named_arg(zend_string *arg_name, tco_context *context)
{
    zend_op_array *op_array = context->op_array;

    for (uint32_t i = 0; i < op_array->num_args; i++) {
        if (zend_string_equals(op_array->arg_info[i].name, arg_name)) {
            return i;
        }
    }

    // (We should never be able to get this far.)

    return 0;
}

/*
 * Within the array allocated for new assigment opcodes, this will initialise
 * the next opcode to an assignment.
 *
 * Only the first operand is set i.e. which variable the assignment should be
 * writing to, for the given argument number/index.
 *
 * The 2nd operand (the value to assign) should be set subsequently, elsewhere.
 */
inline zend_op *tco_init_set_arg_op(
    tco_op_block *op_block,
    zend_op_array *op_array,
    uint32_t arg_index
) {
    zend_op *new_op = &op_block->pseudo_ops[op_block->next_pseudo_op++];

    new_op->opcode = ZEND_ASSIGN;

    new_op->op1_type = IS_CV;
    new_op->op1.var = TCO_ARG_RECV_OPCODE(op_array, arg_index).result.var;

    SET_UNUSED(new_op->result);

    return new_op;
}

/*
 * This function will analyse the opcodes for a given ...
 */
void tco_analyse_call(tco_op_block *op_block, tco_context *context)
{
    /*
     * What we essentially do in this pass is:
     *
     * - Identify which T vars need to be preserved (i.e. those passed as arguments).
     * - Track how much additional space will be needed for the modified opcodes.
     * - Rewrite the opcodes to remove the init/call/return/etc.
     * - Write the opcodes to assign values to variables used for arguments and
     *   jump back to the beginning of the function (after the recv opcodes).
     * - Where the recursive calls would have been, we'll instead add
     *   a jump to the aforementioned opcodes.
     *
     * (Not necessarily in that order.)
     */

    zend_op *op;
    zend_op *new_op;

    zend_op_array *op_array = context->op_array;

    // If we got this far, optimisation is going to happen.

    context->do_optimise = true;

    // This array will be used to flag which arg indices have been explicitly set.
    // (And by extension: which arguments remain to be set afterwards.)

    bool *args_set = calloc(op_array->num_args, sizeof(args_set));

    // T variable mapping.
    // (op_array->T itself will be updated elsewhere.)

    tco_t_remap *t_remaps = tco_get_t_remaps(context);

    uint32_t next_free_t_var = op_array->T;

    // I won't yet do any shenanigans to optimise this part;
    // (I also won't yet account for default values.)

    uint32_t next_arg_index = 0;

    uint32_t destination_index = op_block->op_array_start_index;

    // This loop will skip the init at the first index & the call and return at the end.

    uint32_t end_index = (op_block->op_array_end_index - 2);

    for (
        uint32_t i = (op_block->op_array_start_index + 1);
        i <= end_index;
        i++
    ) {
        op = &op_array->opcodes[i];

        /*
         * If this opcode is trying to read a T var (in either operand)
         * we need to ensure it's reading from the remapped T var (if applicable).
         *
         * If the opcode is trying to alter a protected T var, we need to remap it.
         *
         * We should be safe to assume that no opcode will be trying to access
         * the new T vars - given that they didn't exist until we just created them.
         *
         * This part needs to be done irrespective of the opcode in question - and
         * it's important that it's done first i.e. before anything else.
         */

        tco_do_operand_remaps(op->op1_type, op->op1, t_remaps);
        tco_do_operand_remaps(op->op2_type, op->op2, t_remaps);

        tco_do_operand_remaps(op->result_type, op->result, t_remaps);

        /*
         * Certain opcodes require additional processing.
         *
         * (Todo: Elaborate further here on next_arg_index, etc.)
         */

        switch (op->opcode) {
            case ZEND_CHECK_UNDEF_ARGS:
                // This opcode isn't needed.

                tco_nop_out(op);

                break;

            case ZEND_SEND_VAR_EX:
            case ZEND_SEND_VAL_EX:
            case ZEND_SEND_VAR:
            case ZEND_SEND_VAL:
                // If I'm not wrong, operand 2 being a constant means it's a named argument.
                // Otherwise, operand 2 is the argument #... possibly?

                if (op->op2_type == IS_CONST) {
                    // In theory, this function should never fail to find a value.

                    next_arg_index = tco_find_named_arg(
                        Z_STR_P(CT_CONSTANT_EX(op_array, op->op2.constant)),
                        context
                    );
                } else {
                    // (The indices in the operand are 1-based - so we'll have to subtract 1.)

                    next_arg_index = op->op2.num - 1;
                }

                // Flag this argument as having been set explicitly.

                args_set[next_arg_index] = true;

                // Here we'll exploit the RECV/RECV_INIT opcodes to get the appropriate CV values.

                new_op = tco_init_set_arg_op(op_block, op_array, next_arg_index);

                new_op->op2_type = IS_TMP_VAR;
                new_op->op2.var = op->op1.var;

                // ...

                // Any T variable used here needs to be protected.
                // (It's going to be needed later.)

                if (op->op1_type == IS_TMP_VAR) {
                    t_remaps[op->op1.var] = next_free_t_var++;
                }

                // Increment the argument counter.

                ++next_arg_index;

                // Nop out this opcode also.

                tco_nop_out(op);

                break;

            default:
                // For all other opcodes, copy the opcode to its new location
                // (and increment destination_index for the next opcode)

                op_array->opcodes[destination_index++] = *op;

                break;
        }
    }

    // Now we'll iterate over every argument - and if it wasn't already set,
    // we'll set it to its default value.

    for (uint32_t i = 0; i < op_array->num_args; i++) {
        if (!args_set[i]) {
            new_op = tco_init_set_arg_op(op_block, op_array, i);

            new_op->op2_type = IS_CONST;
            new_op->op2.constant = TCO_ARG_RECV_OPCODE(op_array, i).op2.constant;
        }
    }

    // This isn't strictly necessary, but we'll nop out the unused opcodes.
    // (They're more likely to get optimised out later if they're nopped.)

    tco_nop_out(&op_array->opcodes[op_block->op_array_end_index]);
    tco_nop_out(&op_array->opcodes[op_block->op_array_end_index - 1]);

    // We'll need to allocate more memory for the new opcodes & a jump.
    // (We'll assume that every arg needs to be set - so we'll use op_array->num_args for the count.)

    context->required_opcode_count += (op_array->num_args + 1);

    // The final opcode here needs to be a jump to where the new code will eventually be.

    tco_make_jmp(&op_array->opcodes[destination_index], context->appendix_offset);

    // Update the "appendix offset" ready for the next block (if any).

    context->appendix_offset += op_array->num_args;

    // Free allocated memory, etc.

    free(args_set);
}

/*
 * The general idea here is:
 *
 * - Start from the end of the opcode array.
 * - Whenever a return opcode is encountered, be on red alert.
 * - If the next (technically previous) opcode is a function call...
 * - Start gathering useful intel.
 * -
 */
void tco_explore_op_array(tco_context *context)
{
    tco_op_block *current_op_block;
    zend_op *op;
    uint32_t return_index;

    zend_op_array *op_array = context->op_array;

    uint32_t search_state = TCO_STATE_SEEKING_RETURN;

    uint32_t i = op_array->last;

    // I think all op arrays are guaranteed to have at least one opcode, but just in case...

    if (i < 1) {
        // If there are no opcodes, there's nothing to do.

        return;
    }

    // Create a starting op block.

    current_op_block = tco_new_op_block(context);

    // For now, assume that the starting block ends at the end of the op array.
    // (We can also assume that the start is at the beginning.)

    current_op_block->op_array_end_index = i - 1;
    current_op_block->op_array_start_index = 0;

    // Now iterate over the op array.

    do {
        --i;

        op = &op_array->opcodes[i];

        switch (op->opcode) {
            case ZEND_RETURN:
                search_state = TCO_STATE_SEEKING_CALL;
                return_index = i;

                break;

            case ZEND_DO_UCALL:
            case ZEND_DO_FCALL:
            case ZEND_DO_FCALL_BY_NAME:
                if (search_state == TCO_STATE_SEEKING_CALL) {
                    search_state = TCO_STATE_SEEKING_INIT;
                }

                break;

         // case ZEND_INIT_NS_FCALL_BY_NAME:
            case ZEND_INIT_METHOD_CALL:
            case ZEND_INIT_STATIC_METHOD_CALL:
            case ZEND_INIT_FCALL:
            case ZEND_INIT_FCALL_BY_NAME:
                // Determine whether this is a recursive call.

                if (tco_is_call_recursive(op_array, op)) {
                    /*
                     * If we got this far, every opcode between here & return_index
                     * needs to be placed in its own block.
                     */

                    /* If the end index of the current block is the same as return_index,
                     * we can reuse the current block (as nothing else is in it).
                     * Otherwise, we'll have to create a new one.
                     */

                    if (current_op_block->op_array_end_index != return_index) {
                        // Set the start index of the current block to whatever comes after return_index.

                        current_op_block->op_array_start_index = return_index + 1;

                        // Create a new block for the recursive call.

                        current_op_block = tco_new_op_block(context);
                    }

                    // Point the current block (whether it be new or old) to the recursive call opcodes.

                    current_op_block->op_array_end_index = return_index;
                    current_op_block->op_array_start_index = i;

                    // Here we need to run some analysis on the call - so we know in advance
                    // what will need to be rewritten later (in tco_assemble_blocks).

                    tco_analyse_call(current_op_block, context);

                    // Create a new block for whatever comes next.
                    // (Which we also only need to do if we know there's still more opcodes to process.)

                    if (i > 0) {
                        current_op_block = tco_new_op_block(context);

                        current_op_block->op_array_end_index = i - 1;
                        current_op_block->op_array_start_index = 0;
                    }
                }

                // Either way, reset the search state.

                search_state = TCO_STATE_SEEKING_RETURN;

                break;

            case ZEND_RECV_INIT:
            case ZEND_RECV:
                // As we're searching backwards, the first occurrence of
                // any of these opcodes is our cue to leave.

                context->start_address = i + 1;

                goto __done;

            default:
                /*
                 * If we're here, the current opcode is neither a return,
                 * a call or an init.
                 *
                 * If the current search state is TCO_STATE_SEEKING_CALL, we need
                 * to reset to TCO_STATE_SEEKING_RETURN (because this means we found
                 * a return, but no immediate call i.e. this cannot be a tail call).
                 */

                if (search_state == TCO_STATE_SEEKING_CALL) {
                    search_state = TCO_STATE_SEEKING_RETURN;
                }
        }
    } while (i);

    __done:
        return;
}

/* ... */
static void tco_op_handler(zend_op_array *op_array)
{
    // If there's no function name, we ain't interested.

    if (!op_array->function_name) {
        return;
    }

    tco_context *context = tco_new_context(op_array);

    tco_explore_op_array(context);

    if (context->do_optimise) {
        tco_assemble_blocks(context);
    }

    tco_free_context(context);
}

/*
 */
void tco_cleanup()
{

}

/* Main startup function for the extension. */
static void tco_startup(void)
{
    dbg = fopen("G:/dev/tailcall/astlog.txt", "w");
}

/* Zend extension jazz */

ZEND_EXT_API zend_extension zend_extension_entry = {
    "Tail call optimisation",
    "0.1",
    "Terence C.",
    NULL,
    NULL,
    NULL,
    NULL,
    tco_startup,
    NULL,
    NULL,
    tco_op_handler,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    STANDARD_ZEND_EXTENSION_PROPERTIES
};

ZEND_EXTENSION();