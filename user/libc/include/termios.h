#ifndef USER_TERMIOS_H
#define USER_TERMIOS_H

typedef unsigned char cc_t;
typedef unsigned int  tcflag_t;

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t     c_cc[32];
};

#define ICANON  0x0002
#define ECHO    0x0008
#define ISIG    0x0001
#define VTIME   5
#define VMIN    6

#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

int tcgetattr(int fd, struct termios *t);
int tcsetattr(int fd, int opt, const struct termios *t);

#endif