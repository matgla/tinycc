/* Phase D lever: R9 GOT-base save/restore around calls.
 * With -mpic-data-is-text-relative the backend treats R9 as the GOT base.  It
 * used to save and restore it around every call; Phase 1 of
 * plan_binary_size_reduction.md hoisted the save, so the shape to hold now is
 * one store in the prologue and one reload after each call.
 */
int callee(int x);

int caller(int x) {
    return callee(x + 1) + callee(x + 2);
}
