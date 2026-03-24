/*void gen_pawn(const Pos* p, int from, int white, Move* moves, int* n) {
    int r = (from / 8) + 1;
    //int f = from % 8;
    int to = moves[*n].to;
    char toch = p->b[to];
    char promo = moves[*n].promo;

    if (white == 1) { //check if it's a white pawn

        //check if slot one square ahead is empty
        if ((to == (from + 8) && (toch == '.'))) {
            add_move(moves, n, from, to, promo);
        }

        //check if it's pawn's 1st move by checking its rank, then see if slot two squares ahead is empty
        if ((r == 2) && (to == (from + 16)) && (toch == '.')) {
            add_move(moves, n, from, to, promo);
        }

        //check if left diagonal square contains an enemy
        if ((to == (from + 7)) && (is_black(toch))) {
            add_move(moves, n, from, to, promo);
        }

        //check if right diagonal square contains an enemy
        if ((to == (from + 9)) && (is_black(toch))) {
            add_move(moves, n, from, to, promo);
        }
    }

    else if (white == 0) { //check if it's a black pawn

        //check if one square ahead is empty
        if ((to == (from - 8) && (toch == '.'))) {
            add_move(moves, n, from, to, promo);
        }

        //check if it's pawn's 1st move by checking its rank, then see if slot two squares ahead is empty
        if ((r == 7) && (to == (from - 16)) && (toch == '.')) {
            add_move(moves, n, from, to, promo);
        }

        //check if right diagonal square contains an enemy
        if ((to == (from - 7)) && !(is_black(toch))) {
            add_move(moves, n, from, to, promo);
        }

        //check if left diagonal square contains an enemy
        if ((to == (from - 9)) && !(is_black(toch))) {
            add_move(moves, n, from, to, promo);
        }
    }

}*/