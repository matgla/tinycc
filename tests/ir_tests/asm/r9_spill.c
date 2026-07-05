/* Phase D lever: R9 GOT-base save/restore around calls.
 * With -mpic-data-is-text-relative the backend treats R9 as the GOT base and
 * currently saves/restores it around every call.  This test characterizes the
 * current behavior; once Phase 1 of plan_binary_size_reduction.md lands it
 * should be flipped to assert the absence of these saves.
 */
int callee(int x);

int caller(int x) {
    return callee(x + 1) + callee(x + 2);
}
