// SPDX-License-Identifier: Apache-2.0
// The unreachable new-expression of ct_new_delete.cpp, built with --ct-autofree: the runtime
// reports it as unreachable and releases it at once, so no leak remains at exit.
int main()
{
    new int;
    return 0;
}
