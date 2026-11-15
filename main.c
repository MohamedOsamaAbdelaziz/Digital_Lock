/*
 * Digital Lock - Full firmware (ATmega32 @ 16MHz)
 *
 * Two independent ways to open the lock (no priority between them):
 *   1) Phone (HM-10 BLE) sends the secret text  -> PHONE_TOKEN
 *   2) Keypad: type the 4-digit PIN (stored in EEPROM, default 1234)
 *
 * Either one -> relay ON for UNLOCK_MS, LCD shows OPEN, then closes again.
 * 3 wrong PINs -> keypad locked for LOCKOUT_S seconds (phone still works).
 *
 * Keypad keys: 0-9 digits | * clear | D change PIN (asks current PIN first)
 *
 * Wiring: LCD I2C SCL=PC0 SDA=PC1 | Keypad PA0-PA7 | Relay+LED PB0
 *         Buzzer PD5 | HM-10 TXD->PD0, RXD<-PD1 (9600, via divider)
 */
#define F_CPU 16000000UL
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/eeprom.h>
#include <util/delay.h>
#include <stdint.h>
#include <string.h>

/* ---------------- Settings ---------------- */
#define PHONE_TOKEN   "VOICEOK"    /* UPPERCASE, no spaces/underscores. The phone may send
                                      "voice ok", "VOICE_OK", "voice-ok"... all match */
#define DEBUG_RX      1            /* 1 = show last received char at LCD bottom-right.
                                      Set to 0 when everything works */
#define DEFAULT_PIN   "1234"
#define PIN_LEN       4
#define MAX_TRIES     3
#define LOCKOUT_S     30
#define UNLOCK_MS     5000
#define PIN_TIMEOUT   15000        /* ms with no key press -> back to idle */

#define RELAY_PIN     PB0
#define BUZZER_PIN    PD5

#define LCD_BL 0x08
#define LCD_EN 0x04
#define LCD_RS 0x01

/* ---------------- EEPROM ---------------- */
static uint8_t EEMEM ee_magic;
static uint8_t EEMEM ee_pin[PIN_LEN];
#define EE_MAGIC 0xA5

static uint8_t lcd_addr;

/* ---------------- TWI (I2C) ---------------- */
static void twi_init(void) { TWSR = 0; TWBR = 72; }   /* 100 kHz @ 16 MHz */

static uint8_t twi_wait(void) {
    uint16_t t = 60000;
    while (!(TWCR & (1 << TWINT))) { if (!--t) return 0xFF; }
    return TWSR & 0xF8;
}
static uint8_t twi_start(void) {
    TWCR = (1 << TWINT) | (1 << TWSTA) | (1 << TWEN);
    return twi_wait();
}
static uint8_t twi_write(uint8_t b) {
    TWDR = b;
    TWCR = (1 << TWINT) | (1 << TWEN);
    return twi_wait();
}
static void twi_stop(void) {
    TWCR = (1 << TWINT) | (1 << TWEN) | (1 << TWSTO);
    _delay_us(10);
}
static uint8_t twi_probe(uint8_t addr) {
    uint8_t ok = 0;
    twi_start();
    if (twi_write(addr << 1) == 0x18) ok = 1;
    twi_stop();
    return ok;
}

/* ---------------- LCD over PCF8574 ---------------- */
static void pcf_write(uint8_t d) {
    twi_start();
    twi_write(lcd_addr << 1);
    twi_write(d);
    twi_stop();
}
static void lcd_nib(uint8_t hi, uint8_t rs) {
    uint8_t d = (hi & 0xF0) | LCD_BL | rs;
    pcf_write(d | LCD_EN);
    _delay_us(2);
    pcf_write(d);
    _delay_us(50);
}
static void lcd_send(uint8_t b, uint8_t rs) {
    lcd_nib(b, rs);
    lcd_nib(b << 4, rs);
}
static void lcd_cmd(uint8_t c) {
    lcd_send(c, 0);
    if (c < 4) _delay_ms(2);
}
static void lcd_putc(char c) { lcd_send((uint8_t)c, LCD_RS); }
static void lcd_goto(uint8_t row, uint8_t col) {
    lcd_cmd(0x80 | ((row ? 0x40 : 0x00) + col));
}
static void lcd_line(uint8_t row, const char *s) {   /* print + pad to 16 chars */
    uint8_t n = 0;
    lcd_goto(row, 0);
    while (*s && n < 16) { lcd_putc(*s++); n++; }
    while (n < 16) { lcd_putc(' '); n++; }
}
static void lcd_init(void) {
    _delay_ms(50);
    lcd_nib(0x30, 0); _delay_ms(5);
    lcd_nib(0x30, 0); _delay_ms(1);
    lcd_nib(0x30, 0); _delay_ms(1);
    lcd_nib(0x20, 0); _delay_ms(1);
    lcd_cmd(0x28);
    lcd_cmd(0x0C);
    lcd_cmd(0x01);
    lcd_cmd(0x06);
}

/* ---------------- Keypad ---------------- */
static const char keymap[4][4] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'}
};
static void keypad_init(void) {
    DDRA  = 0x0F;
    PORTA = 0xFF;
}
static char keypad_scan(void) {
    for (uint8_t r = 0; r < 4; r++) {
        PORTA = 0xF0 | (0x0F & ~(1 << r));
        _delay_us(10);
        uint8_t cols = (PINA >> 4) & 0x0F;
        for (uint8_t c = 0; c < 4; c++) {
            if (!(cols & (1 << c))) {
                _delay_ms(20);
                while (!(((PINA >> 4) & 0x0F) & (1 << c)));
                _delay_ms(20);
                PORTA = 0xFF;
                return keymap[r][c];
            }
        }
    }
    PORTA = 0xFF;
    return 0;
}

/* ---------------- UART (HM-10) with RX interrupt ---------------- */
#define RB_SIZE 32
static volatile char    rb[RB_SIZE];
static volatile uint8_t rb_h = 0, rb_t = 0;

ISR(USART_RXC_vect) {
    char c = UDR;
    uint8_t n = (rb_h + 1) & (RB_SIZE - 1);
    if (n != rb_t) { rb[rb_h] = c; rb_h = n; }
}
static void uart_init(void) {
    UBRRH = 0;
    UBRRL = 103;                                   /* 9600 @ 16 MHz */
    UCSRB = (1 << RXEN) | (1 << TXEN) | (1 << RXCIE);
    UCSRC = (1 << URSEL) | (1 << UCSZ1) | (1 << UCSZ0);
}
static void uart_putc(char c) {
    while (!(UCSRA & (1 << UDRE)));
    UDR = c;
}
static void uart_puts(const char *s) { while (*s) uart_putc(*s++); }

static char rx_buf[24];
static uint8_t rx_idx = 0;

static void phone_reset(void) {
    rb_t = rb_h;
    rx_idx = 0;
    rx_buf[0] = 0;
}
/* returns 1 when the secret token has been received from the phone */
static uint8_t phone_token_received(void) {
    while (rb_t != rb_h) {
        char c = rb[rb_t];
        rb_t = (rb_t + 1) & (RB_SIZE - 1);
#if DEBUG_RX
        if (c != '\n' && c != '\r') {              /* last 6 received chars, bottom-right */
            static char dbg[7] = "      ";
            for (uint8_t i = 0; i < 5; i++) dbg[i] = dbg[i + 1];
            dbg[5] = (c >= 32 && c < 127) ? c : '?';
            lcd_goto(1, 10);
            for (uint8_t i = 0; i < 6; i++) lcd_putc(dbg[i]);
        }
#endif
        if (c == '\n' || c == '\r') { rx_idx = 0; rx_buf[0] = 0; continue; }
        if (c == ' ' || c == '_' || c == '-') continue;   /* ignore separators */
        if (c >= 'a' && c <= 'z') c -= 32;                /* case-insensitive */
        if (rx_idx >= sizeof(rx_buf) - 1) rx_idx = 0;
        rx_buf[rx_idx++] = c;
        rx_buf[rx_idx] = 0;
        if (strstr(rx_buf, PHONE_TOKEN)) {
            rx_idx = 0; rx_buf[0] = 0;
            return 1;
        }
    }
    return 0;
}

/* ---------------- Buzzer / Relay ---------------- */
static void delay_ms_var(uint16_t ms) { while (ms--) _delay_ms(1); }
static void beep(uint16_t ms) {
    PORTD |= (1 << BUZZER_PIN);
    delay_ms_var(ms);
    PORTD &= ~(1 << BUZZER_PIN);
}
static void relay(uint8_t on) {
    if (on) PORTB |= (1 << RELAY_PIN);
    else    PORTB &= ~(1 << RELAY_PIN);
}

/* ---------------- PIN handling ---------------- */
static void pin_load_defaults(void) {
    if (eeprom_read_byte(&ee_magic) != EE_MAGIC) {
        const char *d = DEFAULT_PIN;
        for (uint8_t i = 0; i < PIN_LEN; i++) eeprom_update_byte(&ee_pin[i], d[i]);
        eeprom_update_byte(&ee_magic, EE_MAGIC);
    }
}
static uint8_t pin_check(const char *in) {
    for (uint8_t i = 0; i < PIN_LEN; i++)
        if (eeprom_read_byte(&ee_pin[i]) != (uint8_t)in[i]) return 0;
    return 1;
}
static void pin_save(const char *in) {
    for (uint8_t i = 0; i < PIN_LEN; i++) eeprom_update_byte(&ee_pin[i], in[i]);
}

static void show_idle(void) {
    lcd_line(0, "Door: CLOSED");
    lcd_line(1, "PIN or Phone");
}

/*
 * Reads a PIN from the keypad.
 * idle_mode = 1 : main screen; also watches the phone; waits forever for 1st key
 * returns: 1 = PIN entered, 0 = timeout, -1 = 'D' pressed, 2 = phone token received
 */
static int8_t read_pin(char *buf, uint8_t allow_d, uint8_t idle_mode) {
    uint8_t n = 0;
    uint16_t t = 0;
    if (!idle_mode) lcd_line(1, "");
    while (1) {
        if (idle_mode && phone_token_received()) return 2;

        char k = keypad_scan();
        if (k) {
            t = 0;
            beep(30);
            if (k >= '0' && k <= '9' && n < PIN_LEN) {
                if (idle_mode && n == 0) {
                    lcd_line(0, "PIN (D=change):");
                    lcd_line(1, "");
                }
                buf[n++] = k;
                lcd_goto(1, n - 1);
                lcd_putc('*');
            } else if (k == '*') {
                n = 0;
                if (idle_mode) show_idle(); else lcd_line(1, "");
            } else if (k == 'D' && allow_d && n == 0) {
                return -1;
            }
        }
        if (n == PIN_LEN) { _delay_ms(200); return 1; }

        _delay_ms(10);
        if (!idle_mode || n > 0) {
            t += 10;
            if (t >= PIN_TIMEOUT) return 0;
        }
    }
}

/* ---------------- Actions ---------------- */
static void do_unlock(void) {
    uart_puts("OK\r\n");
    lcd_line(0, "Door: OPEN");
    lcd_line(1, "Welcome!");
    relay(1);
    beep(150);
    delay_ms_var(UNLOCK_MS - 150);
    relay(0);
    lcd_line(0, "Door: CLOSED");
    beep(80);
    _delay_ms(500);
}

static void do_lockout(void) {
    uart_puts("LOCKED\r\n");
    beep(1000);
    for (int8_t s = LOCKOUT_S; s > 0; s--) {
        char msg[17];
        msg[0] = 'W'; msg[1] = 'a'; msg[2] = 'i'; msg[3] = 't'; msg[4] = ' ';
        msg[5] = '0' + (s / 10); msg[6] = '0' + (s % 10); msg[7] = 's'; msg[8] = 0;
        lcd_line(0, "Keypad LOCKED");
        lcd_line(1, msg);
        for (uint8_t i = 0; i < 10; i++) {          /* phone still works */
            _delay_ms(100);
            if (phone_token_received()) { do_unlock(); return; }
        }
    }
}

static void do_change_pin(uint8_t *tries) {
    char cur[PIN_LEN], nw[PIN_LEN];
    lcd_line(0, "Current PIN:");
    if (read_pin(cur, 0, 0) != 1) return;
    if (!pin_check(cur)) {
        beep(500);
        lcd_line(0, "Wrong PIN");
        lcd_line(1, "");
        _delay_ms(1000);
        (*tries)++;
        return;
    }
    lcd_line(0, "New PIN:");
    if (read_pin(nw, 0, 0) != 1) return;
    pin_save(nw);
    lcd_line(0, "PIN saved");
    lcd_line(1, "");
    beep(100); _delay_ms(100); beep(100);
    _delay_ms(1000);
}

int main(void) {
    DDRB |= (1 << RELAY_PIN);  PORTB &= ~(1 << RELAY_PIN);
    DDRD |= (1 << BUZZER_PIN); PORTD &= ~(1 << BUZZER_PIN);

    keypad_init();
    uart_init();
    twi_init();
    _delay_ms(100);

    if (twi_probe(0x27))      lcd_addr = 0x27;
    else if (twi_probe(0x3F)) lcd_addr = 0x3F;
    else {
        while (1) {                        /* LCD not found: 3 beeps loop */
            for (uint8_t i = 0; i < 3; i++) { beep(150); _delay_ms(150); }
            _delay_ms(1000);
        }
    }

    lcd_init();
    pin_load_defaults();
    sei();
    beep(200);

    uint8_t tries = 0;

    while (1) {
        char pin[PIN_LEN];

        relay(0);
        show_idle();
        phone_reset();

        int8_t r = read_pin(pin, 1, 1);

        if (r == 2) {                       /* phone opened it */
            tries = 0;
            do_unlock();
        } else if (r == 1) {                /* PIN typed */
            if (pin_check(pin)) {
                tries = 0;
                do_unlock();
            } else {
                tries++;
                uart_puts("WRONG\r\n");
                beep(500);
                lcd_line(0, "Wrong PIN");
                lcd_line(1, "");
                _delay_ms(1000);
            }
        } else if (r == -1) {               /* D = change PIN */
            do_change_pin(&tries);
        }
        /* r == 0: timeout -> back to idle screen */

        if (tries >= MAX_TRIES) {
            do_lockout();
            tries = 0;
        }
    }
}