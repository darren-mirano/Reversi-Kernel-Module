#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/miscdevice.h>
#include <linux/device.h>
#include <linux/rwsem.h>

MODULE_LICENSE("GPL");

/*Necessary kernel module functions*/
static int reversi_open(struct inode *inodep, struct file *filep);
static int reversi_release(struct inode *inodep, struct file *filep);
static ssize_t reversi_read(struct file *filep, char __user *ubuf,
                            size_t count, loff_t *ppos);

static ssize_t reversi_write(struct file *filep, const char __user *ubuf, 
                             size_t count, loff_t *ppos);

/*This function branches off of check_adj_cells, it flips the pieces if a 
  valid move is found. empty_row and empty_col are the potential spot a piece
  can be placed. opp_row and opp_col are the spot where an opponent's piece
  was found, meaning I know what direction to look. Validate is used for checking 
  if there are any valid moves, used when a player/bot has to pass their turn.*/
int check_and_flip(int empty_row, int empty_col, int opp_row, int opp_col,
                   char piece, int validate);

/*This function checks all 8 surrounding cells. If any of them is an opponent's
  piece, call check_and_flip. Again, validate is used for checking if a turn
  can be passed. */
int check_adj_cells(int row, int col, char piece, int validate);

/*Prints output to userspace*/
void output(char* string, int length); 

/*Checks if there are any valid moves for the current player*/
int check_for_valid_moves(char piece);

/*Checks if the game has ended*/
int check_game_end(void);

/*Counts the pieces and determines a winner if the game has ended*/
int count_pieces(void);

/*Main function to run the game*/
int start(int length);

char kern_buf [120];
char gameboard[8][8];
char turn;
char player;
char bot;
int game_flag;
int game_print_end;

static DECLARE_RWSEM(lock);

static const struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = reversi_open,
    .release = reversi_release,
    .read = reversi_read,
    .write = reversi_write,
    .llseek = no_llseek,
};

static struct miscdevice reversi_device = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "reversi",
    .fops = &fops,
    .mode = 0666, /*Gives correct permissions*/
};

/*Initialization function for the device*/
static int __init reversi_init(void){

    int check;
    check = misc_register(&reversi_device);
    if(check != 0){
        printk(KERN_ALERT"ERROR!\n");
        return check;
    }

    return 0;
}

/*Uninitialization function*/
static void __exit reversi_exit(void){
    misc_deregister(&reversi_device);
}

/*Function that runs when the device is opened*/
static int reversi_open(struct inode *inodep, struct file *filep){
    printk(KERN_ALERT"Reversi device opened\n");
    game_flag = 0;
    return 0;
}

/*Function that runs when device is closed*/
static int reversi_release(struct inode *inodep, struct file *filep){
    printk(KERN_ALERT"Reversi device released\n");
    return 0;
}

/*Device read function*/
static ssize_t reversi_read(struct file *filep, char __user *ubuf, size_t count, loff_t *ppos){

    int var; 
    down_read(&lock);
    if(count > sizeof(kern_buf)){
        count = sizeof(kern_buf);
    }

    var = copy_to_user(ubuf, kern_buf, count);
    up_read(&lock);

    return 67;
}

/*Device write function*/
static ssize_t reversi_write(struct file *filep, const char __user *ubuf, size_t count, loff_t *ppos){
    int var;
    
    down_write(&lock);
    if (count > sizeof(kern_buf)){
        count = sizeof(kern_buf);
    }

    var = copy_from_user(kern_buf, ubuf, count);
    start(count);
    printk("Count is %zu\n", count);
    up_write(&lock);

    return count;
}

void output(char* string, int length){
    int index = 0;
    int size = 80;
    
    for (index = 0; index < length; index++){
        kern_buf[index] = string[index];
    }

    for(index = length; index < size; index++){
        kern_buf[index] = 0;
    }
}

int start(int length){
    /*Command always has a 0 in front*/
    if (kern_buf[0] != '0'){
        output("INVFMT", 6);
        return -1;
    }

    /*Command cannot be longer than 7*/
    if (length > 7){
        output("INVFMT", 6);
        return -1;
    }

    /*Start game command (00)*/
    if (kern_buf[1] == '0'){
        int i;
        int j;
        
        if (kern_buf[2] != ' '){
            output("INVFMT", 6);
            return -1;
        }
        if (kern_buf[3] != 'X' && kern_buf[3] != 'O'){
            output("INVFMT", 6);
            return -1;
        }

        player = kern_buf[3];
        if (player == 'X'){
            bot = 'O';
        } else {
            bot = 'X';
        }

        turn = 'X'; /*X always goes first*/

        for(i = 0; i < 8; i++){
            for (j = 0; j < 8; j++){
                gameboard[i][j] = '-';
            }
        }

        /*Set starting pieces*/
        gameboard[3][3] = 'O';
        gameboard[3][4] = 'X';
        gameboard[4][3] = 'X';
        gameboard[4][4] = 'O';

        game_flag = 1;
        game_print_end = 0;

        output("OK", 2);

    /*Print board command (01)*/
    } else if (kern_buf[1] == '1'){
        int i;
        int j;
        int index;
        char print_buf[67];
        
        if (kern_buf[2] != '\n'){
            output("INVFMT", 6);
            return -1;
        }

        if (game_flag == 0 && game_print_end != 1){
            output("NO GAME", 7);
            return -1;
        }

        index = 0;

        for(i = 0; i < 8; i++){
            for(j = 0; j < 8; j++){
                print_buf[index] = gameboard[i][j];
                index++;
            }
        }

        print_buf[64] = '\t';
        print_buf[65] = turn;
        print_buf[66] = '\n';

        output(print_buf, 67);

    /*Place piece command (02)*/
    } else if (kern_buf[1] == '2'){
        char row_c;
        char col_c;
        int  row;
        int  col;
        int  check;
        int  end;

        check = 0;

        if (kern_buf[2] != ' '){
            output("INVFMT", 6);
            return -1;
        }

        if (kern_buf[4] != ' '){
            output("INVFMT", 6);
            return -1;
        }

        if (kern_buf[6] != '\n'){
            output("INVFMT", 6);
            return -1;
        }

        if (turn != player){
            output("OOT", 3);
            return -1;
        }

        if (game_flag == 0){
            output("NO GAME", 7);
            return -1;
        }

        col_c = kern_buf[3];
        row_c = kern_buf[5];

        col = col_c - 48;
        row = row_c - 48;

        if (col < 0 || col > 7){
            output("ILLMOVE", 7);
        } else if (row < 0 || row > 7){
            output("ILLMOVE", 7);
        } else {
            if (gameboard[row][col] != '-'){
                output("ILLMOVE", 7);
            } else {
                check = check_adj_cells(row, col, turn, 0);
            }

            if (check == 0){
                output("ILLMOVE", 7);
            } else {
                end = 0;
                end = check_game_end();
                if (end == 1){ /*Game is over*/
                    count_pieces();
                    game_flag = 0;
                    game_print_end = 1;
                } else {
                    turn = bot;
                    output("OK", 2);
                }

            }
            
        }

    /*Bot move command (03)*/ 
    } else if (kern_buf[1] == '3'){
        int check;
        int flag;
        int end;
        int i;
        int j;
        
        if (kern_buf[2] != '\n'){
            output("INVFMT", 6);
            return -1;
        }

        if (turn != bot){
            output("OOT", 3);
            return -1;
        }

        if (game_flag == 0){
            output("NO GAME", 7);
            return -1;
        }

        flag = 0;
        check = 0;

        for(i = 0; i < 8; i++){
            for (j = 0; j < 8; j++){
                if (gameboard[i][j] == '-'){
                    check = check_adj_cells(i,j,turn, 0);
                    if (check == 1){
                        end = 0;
                        end = check_game_end();
                        if (end == 1){ /*Game is over*/
                            count_pieces();
                            game_flag = 0;
                            game_print_end = 1;
                        } else {
                            output("OK", 2);
                            turn = player;
                        }
                        break;
                    }
                }
            }
            if (check == 1){
                break;
            }
        }

        if (check == 0){
            output("ILLMOVE", 7);
        }

    /*Skip turn command (04)*/
    } else if (kern_buf[1] == '4'){
        int check;

        if (kern_buf[2] != '\n'){
            output("INVFMT", 6);
            return -1;
        }

        if (game_flag == 0){
            output("NO GAME", 7);
            return -1;
        }

        check = 0;
        check = check_for_valid_moves(turn);

        if (check == 0){
            output("OK", 2);
            if (turn == player){
                turn = bot;
            } else if (turn == bot){
                turn = player;
            }
        } else if (check == 1){
            output("ILLMOVE", 7);
        }
    }
    return 0;
}

int check_adj_cells(int row, int col, char piece, int validate){
    int val; /*Return value, 0 = no board change, 1 = board change*/
    int move; /*Var for checking if a move was made*/
    char opponent;

    val = 0;
    move = 0;

    opponent = 'X';

    if (piece == 'X'){
        opponent = 'O';
    }

    if (row == 0 && col == 0){ /*Top left condition*/
        if (gameboard[row][col+1] == opponent){ /*Right cell*/
            move = check_and_flip(row,col, row, col+1, piece, validate);
            if (move == 1){val = 1;}
        }
        if (gameboard[row+1][col] == opponent){ /*Bottom cell*/
            move = check_and_flip(row, col, row+1, col, piece, validate);
            if (move == 1){val = 1;}
        }
        if (gameboard[row+1][col+1] == opponent){ /*Bottom right cell*/
            move = check_and_flip(row,col, row+1, col+1, piece, validate);
            if (move == 1){val = 1;}
        }

    } else if (row == 0 && col == 7){ /*Top right condition*/
        if (gameboard[row][col-1] == opponent){ /*Left cell*/
            move = check_and_flip(row,col, row, col-1, piece, validate);
            if (move == 1){val = 1;}
        } 
        if (gameboard[row+1][col-1] == opponent){ /*Bottom left cell*/
            move = check_and_flip(row,col, row+1, col-1, piece, validate);
            if (move == 1){val = 1;}
        } 
        if (gameboard[row+1][col] == opponent){ /*Bottom cell*/
            move = check_and_flip(row, col, row+1, col, piece, validate);
            if (move == 1){val = 1;}
        }

    } else if (row == 0 && col != 0 && col != 7){ /*Top row condition*/
        if (gameboard[row][col-1] == opponent){ /*Left cell*/
            move = check_and_flip(row,col, row, col-1, piece, validate);
            if (move == 1){val = 1;}
        }
        if (gameboard[row][col+1] == opponent){ /*Right cell*/
            move = check_and_flip(row,col, row, col+1, piece, validate);
            if (move == 1){val = 1;}
        } 
        if (gameboard[row+1][col-1] == opponent){ /*Bottom left cell*/
            move = check_and_flip(row,col, row+1, col-1, piece, validate);
            if (move == 1){val = 1;}
        } 
        if (gameboard[row+1][col] == opponent){ /*Bottom cell*/
            move = check_and_flip(row, col, row+1, col, piece, validate);
            if (move == 1){val = 1;}
        }
        if (gameboard[row+1][col+1] == opponent){ /*Bottom right cell*/
            move = check_and_flip(row,col, row+1, col+1, piece, validate);
            if (move == 1){val = 1;}
        }

    } else if (row == 7 && col == 0){ /*Bottom left cell condition*/
        if (gameboard[row-1][col] == opponent){ /*Top cell*/
            move = check_and_flip(row, col, row-1, col, piece, validate);
            if (move == 1){val = 1;}
        }
        if (gameboard[row-1][col+1] == opponent){ /*Top right cell*/
            move = check_and_flip(row,col,row-1,col+1, piece, validate);
            if (move == 1){val = 1;}
        }
        if (gameboard[row][col+1] == opponent){ /*Right cell*/
            move = check_and_flip(row,col, row, col+1, piece, validate);
            if (move == 1){val = 1;}
        } 

    } else if (row == 7 && col == 7){ /*Bottom right cell condition*/
        if (gameboard[row-1][col-1] == opponent){ /*Top left cell*/
            move = check_and_flip(row,col,row-1,col-1, piece, validate);
            if (move == 1){val = 1;}
        }
        if (gameboard[row-1][col] == opponent){ /*Top cell*/
            move = check_and_flip(row, col, row-1, col, piece, validate);
            if (move == 1){val = 1;}
        }
        if (gameboard[row][col-1] == opponent){ /*Left cell*/
            move = check_and_flip(row,col, row, col-1, piece, validate);
            if (move == 1){val = 1;}
        } 

    } else if (row == 7 && col != 0 && col != 7){ /*Bottom row condition*/
        if (gameboard[row-1][col-1] == opponent){ /*Top left cell*/
            move = check_and_flip(row,col,row-1,col-1, piece, validate);
            if (move == 1){val = 1;}
        }
        if (gameboard[row-1][col] == opponent){ /*Top cell*/
            move = check_and_flip(row, col, row-1, col, piece, validate);
            if (move == 1){val = 1;}
        }
        if (gameboard[row-1][col+1] == opponent){ /*Top right cell*/
            move = check_and_flip(row,col,row-1,col+1, piece, validate);
            if (move == 1){val = 1;}
        }
        if (gameboard[row][col-1] == opponent){ /*Left cell*/
            move = check_and_flip(row,col, row, col-1, piece, validate);
            if (move == 1){val = 1;}
        }
        if (gameboard[row][col+1] == opponent){ /*Right cell*/
            move = check_and_flip(row,col, row, col+1, piece, validate);
            if (move == 1){val = 1;}
        } 

    } else if (row != 0 && row != 7 && col == 0){ /*Left-most col condition*/
        if (gameboard[row-1][col] == opponent){ /*Top cell*/
            move = check_and_flip(row, col, row-1, col, piece, validate);
            if (move == 1){val = 1;}
        } 
        if (gameboard[row-1][col+1] == opponent){ /*Top right cell*/
            move = check_and_flip(row,col,row-1,col+1, piece, validate);
            if (move == 1){val = 1;}
        } 
        if (gameboard[row][col+1] == opponent){ /*Right cell*/
            move = check_and_flip(row,col, row, col+1, piece, validate);
            if (move == 1){val = 1;}
        }
        if (gameboard[row+1][col] == opponent){ /*Bottom cell*/
            move = check_and_flip(row, col, row+1, col, piece, validate);
            if (move == 1){val = 1;}
        }
        if (gameboard[row+1][col+1] == opponent){ /*Bottom right cell*/
            move = check_and_flip(row,col, row+1, col+1, piece, validate); 
            if (move == 1){val = 1;}
        }

    } else if (row != 0 && row != 7 && col == 7){ /*Right-most col condition*/
        if (gameboard[row-1][col-1] == opponent){ /*Top left cell*/
            move = check_and_flip(row,col,row-1,col-1, piece, validate);
            if (move == 1){val = 1;}
        }
        if (gameboard[row-1][col] == opponent){ /*Top cell*/
            move = check_and_flip(row, col, row-1, col, piece, validate);
            if (move == 1){val = 1;}
        }
        if (gameboard[row][col-1] == opponent){ /*Left cell*/
            move = check_and_flip(row,col, row, col-1, piece, validate);
            if (move == 1){val = 1;}
        }
        if (gameboard[row+1][col-1] == opponent){ /*Bottom left cell*/
            move = check_and_flip(row,col, row+1, col-1, piece, validate);
            if (move == 1){val = 1;}
        } 
        if (gameboard[row+1][col] == opponent){ /*Bottom cell*/
            move = check_and_flip(row, col, row+1, col, piece, validate); 
            if (move == 1){val = 1;}  
        }

    } else { /*Every other cell*/
        if (gameboard[row-1][col-1] == opponent){ /*Top left cell*/
            move = check_and_flip(row,col,row-1,col-1, piece, validate);
            if (move == 1){val = 1;}
        }  
        if (gameboard[row-1][col] == opponent){ /*Top cell*/
            move = check_and_flip(row, col, row-1, col, piece, validate);
            if (move == 1){val = 1;}
        }
        if (gameboard[row-1][col+1] == opponent){ /*Top right cell*/
            move = check_and_flip(row,col,row-1,col+1, piece, validate);
            if (move == 1){val = 1;}
        } 
        if (gameboard[row][col-1] == opponent){ /*Left cell*/
            move = check_and_flip(row,col, row, col-1, piece, validate);
            if (move == 1){val = 1;}
        }
        if (gameboard[row][col+1] == opponent){ /*Right cell*/
            move = check_and_flip(row,col, row, col+1, piece, validate);
            if (move == 1){val = 1;}
        }  
        if (gameboard[row+1][col-1] == opponent){ /*Bottom left cell*/
            move = check_and_flip(row,col, row+1, col-1, piece, validate);
            if (move == 1){val = 1;}
        } 
        if (gameboard[row+1][col] == opponent){ /*Bottom cell*/
            move = check_and_flip(row, col, row+1, col, piece, validate);
            if (move == 1){val = 1;}
        }  
        if (gameboard[row+1][col+1] == opponent){ /*Bottom right cell*/
            move = check_and_flip(row,col, row+1, col+1, piece, validate);
            if (move == 1){val = 1;}
        }
    }
    return val;
}

int check_and_flip(int empty_row, int empty_col, int opp_row, int opp_col, char piece, int validate){
    int return_val;
    char opponent;
    char check;
    int valid;
    int flag;
    int i;

    return_val = 0;
    opponent = 'X';

    if (piece == 'X'){
        opponent = 'O';
    }

    if (opp_row == empty_row - 1 && opp_col == empty_col - 1){ /*Moving up left*/
        check = gameboard[opp_row][opp_col];
        valid = 0;
        flag = 0;
        i = 1;
        while (flag == 0){
            if (opp_row - i < 0 || opp_col - i < 0){
                flag = 1;
            } else if (gameboard[opp_row-i][opp_col-i] == '-'){
                flag = 1;
            } else if (gameboard[opp_row-i][opp_col-i] == piece){
                valid = 1;
                flag = 1;
            } else if (gameboard[opp_row-i][opp_col-i] == opponent){
                i+=1;
            }
        }
        if (validate == 0){
            if (valid == 1){ /*Move is valid, flip pieces*/
                gameboard[empty_row][empty_col] = piece;
                flag = 0;
                i = 1;
                while (flag == 0){
                    if (gameboard[empty_row-i][empty_col-i] == piece){
                        flag = 1;
                    } else {
                        gameboard[empty_row-i][empty_col-i] = piece;
                        i++;
                    }
                }
                return_val = 1;
            }
        } else if (validate == 1){
            if (valid == 1){
                return_val = 1;
            }
        }
    } else if (opp_row == empty_row - 1 && opp_col == empty_col + 1){ /*Moving up right*/
        check = gameboard[opp_row][opp_col];
        valid = 0;
        flag = 0;
        i = 1;
        while (flag == 0){
            if (opp_row - i < 0 || opp_col + i > 7){
                flag = 1;
            } else if (gameboard[opp_row-i][opp_col+i] == '-'){
                flag = 1;
            } else if (gameboard[opp_row-i][opp_col+i] == piece){
                valid = 1;
                flag = 1;
            } else if (gameboard[opp_row-i][opp_col+i] == opponent){
                i+=1;
            }
        }

        if (validate == 0){
            if (valid == 1){ /*Move is valid, flip pieces*/
                gameboard[empty_row][empty_col] = piece;
                flag = 0;
                i = 1;
                while (flag == 0){
                    if (gameboard[empty_row-i][empty_col+i] == piece){
                        flag = 1;
                    } else {
                        gameboard[empty_row-i][empty_col+i] = piece;
                        i++;
                    }
                }
                return_val = 1;
            }
        } else if (validate == 1){
            if(valid == 1){
                return_val = 1;
            }
        }
    } else if (opp_row == empty_row - 1 && opp_col == empty_col){ /*Moving up*/
        check = gameboard[opp_row][opp_col];
        valid = 0;
        flag = 0;
        i = 1;
        while (flag == 0){
            if (opp_row - i < 0){
                flag = 1;
            } else if (gameboard[opp_row-i][opp_col] == '-'){
                flag = 1;
            } else if (gameboard[opp_row-i][opp_col] == piece){
                valid = 1;
                flag = 1;
            } else if (gameboard[opp_row-i][opp_col] == opponent){
                i += 1;
            }
        }

        if (validate == 0){
            if (valid == 1){ /*Move is valid, flip pieces*/
                gameboard[empty_row][empty_col] = piece;
                flag = 0;
                i = 1;
                while (flag == 0){
                    if (gameboard[empty_row-i][empty_col] == piece){
                        flag = 1;
                    } else {
                        gameboard[empty_row-i][empty_col] = piece;
                        i++;
                    }
                }
                return_val = 1;
            }
        } else if (validate == 1){
            if (valid == 1){
                return_val = 1;
            }
        }
    } else if (opp_row == empty_row && opp_col == empty_col - 1){ /*Moving left*/
        check = gameboard[opp_row][opp_col];
        valid = 0;
        flag = 0;
        i = 1;
        while (flag == 0){
            if (opp_col - i < 0){
                flag = 1;
            } else if (gameboard[opp_row][opp_col-i] == '-'){
                flag = 1;
            } else if (gameboard[opp_row][opp_col-i] == piece){
                valid = 1;
                flag = 1;
            } else if (gameboard[opp_row][opp_col-i] == opponent){
                i += 1;
            }
        }

        if (validate == 0){
            if (valid == 1){ /*Move is valid, flip pieces*/
                gameboard[empty_row][empty_col] = piece;
                flag = 0;
                i = 1;
                while (flag == 0){
                    if (gameboard[empty_row][empty_col-i] == piece){
                        flag = 1;
                    } else {
                        gameboard[empty_row][empty_col-i] = piece;
                        i++;
                    }
                }
                return_val = 1;
            }
        } else if (validate == 1){
            if (valid == 1){
                return_val = 1;
            }
        }
    } else if (opp_row == empty_row && opp_col == empty_col + 1){ /*Moving right*/
        check = gameboard[opp_row][opp_col];
        valid = 0;
        flag = 0;
        i = 1;
        while (flag == 0){
            if (opp_col + i > 7){
                flag = 1;
            } else if (gameboard[opp_row][opp_col+i] == '-'){
                flag = 1;
            } else if (gameboard[opp_row][opp_col+i] == piece){
                valid = 1;
                flag = 1;
            } else if (gameboard[opp_row][opp_col+i] == opponent){
                i += 1;
            }
        }

        if (validate == 0){
            if (valid == 1){ /*Move is valid, flip pieces*/
                gameboard[empty_row][empty_col] = piece;
                flag = 0;
                i = 1;
                while (flag == 0){
                    if (gameboard[empty_row][empty_col+i] == piece){
                        flag = 1;
                    } else {
                        gameboard[empty_row][empty_col+i] = piece;
                        i++;
                    }
                }
                return_val = 1;
            }
        } else if (validate == 1){
            if (valid == 1){
                return_val = 1;
            }
        }
    } else if (opp_row == empty_row + 1 && opp_col == empty_col - 1){ /*Moving down left*/
        check = gameboard[opp_row][opp_col];
        valid = 0;
        flag = 0;
        i = 1;
        while (flag == 0){
            if (opp_row + i > 7 || opp_col - i < 0){
                flag = 1;
            } else if (gameboard[opp_row+i][opp_col-i] == '-'){
                flag = 1;
            } else if (gameboard[opp_row+i][opp_col-i] == piece){
                valid = 1;
                flag = 1;
            } else if (gameboard[opp_row+i][opp_col-i] == opponent){
                i+=1;
            }
        }

        if (validate == 0){
            if (valid == 1){ /*Move is valid, flip pieces*/
                gameboard[empty_row][empty_col] = piece;
                flag = 0;
                i = 1;
                while (flag == 0){
                    if (gameboard[empty_row+i][empty_col-i] == piece){
                        flag = 1;
                    } else {
                        gameboard[empty_row+i][empty_col-i] = piece;
                        i++;
                    }
                }
                return_val = 1;
            }
        } else if (validate == 1){
            if (valid == 1){
                return_val = 1;
            }
        }
    } else if (opp_row == empty_row + 1 && opp_col == empty_col){ /*Moving down*/
        check = gameboard[opp_row][opp_col];
        valid = 0;
        flag = 0;
        i = 1;
        while (flag == 0){
            if (opp_row + i > 7){
                flag = 1;
            } else if (gameboard[opp_row+i][opp_col] == '-'){
                flag = 1;
            } else if (gameboard[opp_row+i][opp_col] == piece){
                valid = 1;
                flag = 1;
            } else if (gameboard[opp_row+i][opp_col] == opponent){
                i += 1;
            }
        }

        if (validate == 0){
            if (valid == 1){ /*Move is valid, flip pieces*/
                gameboard[empty_row][empty_col] = piece;
                flag = 0;
                i = 1;
                while (flag == 0){
                    if (gameboard[empty_row+i][empty_col] == piece){
                        flag = 1;
                    } else {
                        gameboard[empty_row+i][empty_col] = piece;
                        i++;
                    }
                }
                return_val = 1;
            }
        } else if (validate == 1){
            if (valid == 1){
                return_val = 1;
            }
        }
    } else if (opp_row == empty_row + 1 && opp_col == empty_col + 1){ /*Moving down right*/
        check = gameboard[opp_row][opp_col];
        valid = 0;
        flag = 0;
        i = 1;
        while (flag == 0){
            if (opp_row + i > 7 || opp_col + i > 7){
                flag = 1;
            } else if (gameboard[opp_row+i][opp_col+i] == '-'){
                flag = 1;
            } else if (gameboard[opp_row+i][opp_col+i] == piece){
                valid = 1;
                flag = 1;
            } else if (gameboard[opp_row+i][opp_col+i] == opponent){
                i+=1;
            }
        }

        if (validate == 0){
            if (valid == 1){ /*Move is valid, flip pieces*/
                gameboard[empty_row][empty_col] = piece;
                flag = 0;
                i = 1;
                while (flag == 0){
                    if (gameboard[empty_row+i][empty_col+i] == piece){
                        flag = 1;
                    } else {
                        gameboard[empty_row+i][empty_col+i] = piece;
                        i++;
                    }
                }
                return_val = 1;
            }
        } else if (validate == 1){
            if (valid == 1){
                return_val = 1;
            }
        }
    }
    return return_val;
}

int check_game_end(){
    int check1;
    int check2;

    check1 = check_for_valid_moves(player);
    check2 = check_for_valid_moves(bot);

    if (check1 == 1 || check2 == 1){
        return 0;
    }

    return 1;
}

int count_pieces(){
    int X;
    int O;
    int i;
    int j;
    X = 0;
    O = 0;

    for (i = 0; i < 8; i++){
        for (j = 0; j < 8; j++){
            if (gameboard[i][j] == 'X'){
                X++;
            } else if (gameboard[i][j] == 'O'){
                O++;
            }
        }
    }

    if (X > O){
        if (player == 'O'){
            output("LOSE", 4);
        } else if (bot == 'O'){
            output("WIN", 3);
        }
    } else if (X < O){
        if (player == 'X'){
            output("LOSE", 4);
        } else if (bot == 'O'){
            output("WIN", 3);
        }
    } else if (X == O){
        output("TIE", 3);
    }
    return 0;
}

int check_for_valid_moves(char piece){
    int i;
    int j;
    for (i = 0; i < 8; i++){
        for (j = 0; j < 8; j++){
            int check;
            if (gameboard[i][j] == '-'){
                check = check_adj_cells(i,j,piece,1);
                if (check == 1){
                    return 1;
                }
            }
        }
    }
    return 0;
}

module_init(reversi_init);
module_exit(reversi_exit);