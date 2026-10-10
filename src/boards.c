/* boards.c - registry of the boards implemented through board.h */
#include "board.h"
static const Board *const ALL[] = {
    &B_VRC1, &B_VRC2_4_21, &B_VRC2_4_22, &B_VRC2_4_23, &B_VRC2_4_25, &B_VRC3, &B_VRC6_24, &B_VRC6_26, &B_VRC7,
    &B_13, &B_16, &B_159, &B_18, &B_19, &B_210, &B_32, &B_33, &B_40, &B_142, &B_64, &B_65, &B_67, &B_68, &B_69,
    &B_78, &B_80, &B_82, &B_185, &B_MMC5,
    NULL
};
const Board *board_find(int mapper) {
    for (int i = 0; ALL[i]; i++) if (ALL[i]->mapper == mapper) return ALL[i];
    return NULL;
}
