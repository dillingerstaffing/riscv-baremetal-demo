// cmds.c: tiny UART command interpreter. Reads lines with echo and
// backspace support, dispatches four commands, and reports unknown
// input instead of ignoring it.

#include "../uart.h"
#include "cmds.h"

#define LINE_MAX 128

// mtime lives in the CLINT on the virt board; same address as in
// src/preempt/clint.c.
#define CLINT_MTIME 0x0200bff8UL

static char line[LINE_MAX];
static int line_len;

// 31 saved general registers, then mepc, mstatus, mcause. Filled by
// trap_entry in trap.S on the ecall issued by snapshot_regs().
extern unsigned long trap_bank[34];

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

// Matches "echo" alone or "echo ..." with one space. Anything else
// starting with "echo" (e.g. "echocardiogram") is a different word.
static int is_echo(const char *s) {
    return streq(s, "echo") || (s[0] == 'e' && s[1] == 'c' && s[2] == 'h' &&
                                s[3] == 'o' && s[4] == ' ');
}

static void cmd_help(void) {
    uart_puts("commands:\n");
    uart_puts("  help    list commands\n");
    uart_puts("  echo    print text back byte-for-byte\n");
    uart_puts("  regs    dump registers saved by the trap handler\n");
    uart_puts("  uptime  print mtime ticks since reset\n");
}

static void cmd_echo(const char *s) {
    // Skip the word "echo" and one following space, then print the
    // rest exactly as typed.
    if (*s == ' ')
        s++;
    uart_puts(s);
    uart_putc('\n');
}

static const char *reg_names[31] = {
    "ra", "sp", "gp", "tp", "t0", "t1", "t2", "s0", "s1",
    "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7",
    "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11",
    "t3", "t4", "t5", "t6",
};

static void snapshot_regs(void) {
    // Trap into trap_entry, which saves x1-x31 at this point, then
    // resumes on the next instruction.
    __asm__ volatile("ecall" ::: "memory");
}

static void cmd_regs(void) {
    int i;
    snapshot_regs();
    for (i = 0; i < 31; i++) {
        uart_puts(reg_names[i]);
        uart_puts(" = ");
        uart_put_hex(trap_bank[i]);
        uart_putc('\n');
    }
    uart_puts("mepc    = ");
    uart_put_hex(trap_bank[31]);
    uart_putc('\n');
    uart_puts("mstatus = ");
    uart_put_hex(trap_bank[32]);
    uart_putc('\n');
    uart_puts("mcause  = ");
    uart_put_hex(trap_bank[33]);
    uart_putc('\n');
}

static void cmd_uptime(void) {
    unsigned long t = *(volatile unsigned long *)CLINT_MTIME;
    uart_puts("mtime = ");
    uart_put_dec(t);
    uart_puts(" ticks\n");
}

static void run_command(void) {
    if (line_len == 0)
        return;
    if (streq(line, "help")) {
        cmd_help();
    } else if (is_echo(line)) {
        cmd_echo(line + 4);
    } else if (streq(line, "regs")) {
        cmd_regs();
    } else if (streq(line, "uptime")) {
        cmd_uptime();
    } else {
        uart_puts("unknown command: ");
        uart_puts(line);
        uart_putc('\n');
    }
}

static void read_line(void) {
    line_len = 0;
    for (;;) {
        char c = uart_getc();
        if (c == '\r' || c == '\n') {
            uart_putc('\n');
            line[line_len] = '\0';
            return;
        }
        if (c == 0x7f || c == 0x08) {  // DEL or BS: erase one char
            if (line_len > 0) {
                line_len--;
                uart_puts("\b \b");
            }
            continue;
        }
        if (c < 0x20)  // ignore other control characters
            continue;
        if (line_len < LINE_MAX - 1) {
            line[line_len++] = c;
            uart_putc(c);  // echo
        }
    }
}

void shell_run(void) {
    uart_puts("uart-shell ready\n");
    for (;;) {
        uart_puts("> ");
        read_line();
        run_command();
    }
}
