#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>

#define PCF_ADDR 0x27
#define LCD_BL   0x08
#define TWBR_VAL ((uint8_t)((F_CPU / 100000UL - 16UL) / 2UL))

#define TWI_START() do { TWCR = (1<<TWINT)|(1<<TWSTA)|(1<<TWEN); while (!(TWCR & (1<<TWINT))); } while (0)
#define TWI_STOP()  do { TWCR = (1<<TWINT)|(1<<TWEN)|(1<<TWSTO); while (TWCR & (1<<TWSTO)); } while (0)

volatile uint16_t distance_passenger = 0;
volatile uint16_t distance_metro = 0;
uint8_t current_state = 0;

static void twi_write_byte(uint8_t b) {
    TWDR = b;
    TWCR = (1<<TWINT)|(1<<TWEN);
    while (!(TWCR & (1<<TWINT)));
}

static void pcf_write(uint8_t data) {
    TWI_START();
    twi_write_byte((uint8_t)(PCF_ADDR << 1 | 0));
    twi_write_byte(data);
    TWI_STOP();
}

static void lcd_write_nibble(uint8_t nibble, uint8_t rs) {
    uint8_t base = (nibble & 0xF0) | LCD_BL | (rs ? 0x01 : 0x00);
    pcf_write((uint8_t)(base | 0x04));
    _delay_us(1);
    pcf_write((uint8_t)(base & ~0x04));
    _delay_us(50);
}

static void lcd_send(uint8_t byte, uint8_t rs) {
    lcd_write_nibble(byte & 0xF0, rs);
    lcd_write_nibble((uint8_t)(byte << 4), rs);
}

static void lcd_command(uint8_t cmd) {
    lcd_send(cmd, 0);
    if (cmd <= 0x03) _delay_ms(2);
}

static void lcd_char(char c) { lcd_send((uint8_t)c, 1); }
static void lcd_string(const char *s) { while (*s) lcd_char(*s++); }

static void lcd_init(void) {
    TWBR = TWBR_VAL; TWSR = 0x00;
    _delay_ms(50);
    lcd_write_nibble(0x30, 0); _delay_ms(5);
    lcd_write_nibble(0x30, 0); _delay_us(150);
    lcd_write_nibble(0x30, 0); _delay_us(150);
    lcd_write_nibble(0x20, 0); _delay_us(150);
    lcd_command(0x28);
    lcd_command(0x0C);
    lcd_command(0x06);
    lcd_command(0x01);
    _delay_ms(2);
}

static void lcd_set_cursor(uint8_t row, uint8_t col) {
    uint8_t addr = (row == 0) ? col : (uint8_t)(0x40 + col);
    lcd_command((uint8_t)(0x80 | addr));
}

void reset_indicators() {
    PORTC &= ~((1 << PORTC0) | (1 << PORTC1) | (1 << PORTC2) | (1 << PORTC3));
}

void red_led_on()    { PORTC |= (1 << PORTC0); }
void yellow_led_on() { PORTC |= (1 << PORTC1); }
void green_led_on()  { PORTC |= (1 << PORTC2); }
void buzzer_on()     { PORTC |= (1 << PORTC3); }

void barrier_open()  { OCR1A = 4800; }
void barrier_close() { OCR1A = 2900; }

void init_io() {
    DDRA |= (1 << DDA0) | (1 << DDA1); 
    DDRB |= (1 << DDB5); 
    DDRC |= (1 << DDC0) | (1 << DDC1) | (1 << DDC2) | (1 << DDC3);
    DDRE &= ~((1 << DDE4) | (1 << DDE5));
}

void init_timer1_servo() {
    TCCR1A = (1 << COM1A1) | (1 << WGM11);
    TCCR1B = (1 << WGM13) | (1 << WGM12) | (1 << CS11);
    ICR1 = 39999;
    barrier_open(); 
}

void init_interrupts_and_timer3() {
    EICRB |= (1 << ISC40) | (1 << ISC50);
    EIMSK |= (1 << INT4) | (1 << INT5);
    TCCR3B = (1 << CS31); 
    sei();
}

ISR(INT4_vect) { if (PINE & (1 << PINE4)) TCNT3 = 0; else distance_passenger = TCNT3 / 116; }
ISR(INT5_vect) { if (PINE & (1 << PINE5)) TCNT3 = 0; else distance_metro = TCNT3 / 116; }

int main(void) {
    init_io();
    init_timer1_servo();
    init_interrupts_and_timer3();
    lcd_init();

    uint16_t m_arrival_limit, m_danger_limit, p_critical_limit, p_warning_limit;

    while (1) {
        PORTA |= (1 << PORTA0) | (1 << PORTA1); 
        _delay_us(10); 
        PORTA &= ~((1 << PORTA0) | (1 << PORTA1));
        _delay_ms(50);

        if (current_state == 4) m_arrival_limit = 20; else m_arrival_limit = 15;
        if (current_state == 3) m_danger_limit = 105; else m_danger_limit = 100;
        if (current_state == 5) p_critical_limit = 55; else p_critical_limit = 50;
        if (current_state == 2) p_warning_limit = 65; else p_warning_limit = 60;

        if (distance_metro > 0 && distance_metro < m_arrival_limit) {
            barrier_open();
            reset_indicators();
            green_led_on();
            if (current_state != 4) {
                lcd_command(0x01); lcd_set_cursor(0,0); lcd_string("TRAIN ARRIVED!");
                lcd_set_cursor(1,0); lcd_string("BOARDING");
                current_state = 4;
            }
        }
        else if (distance_metro > 0 && distance_metro < m_danger_limit) {
            barrier_close();
            reset_indicators();
            red_led_on();
            buzzer_on();
            if (current_state != 3) {
                lcd_command(0x01); lcd_set_cursor(0,0); lcd_string("WARNING!");
                lcd_set_cursor(1,0); lcd_string("TRAIN APPROACH");
                current_state = 3;
            }
        }
        else if (distance_passenger > 0 && distance_passenger < p_critical_limit) {
            barrier_close();
            reset_indicators();
            red_led_on();
            buzzer_on();
            if (current_state != 5) {
                lcd_command(0x01); lcd_set_cursor(0,0); lcd_string("DANGER!");
                lcd_set_cursor(1,0); lcd_string("ZONE VIOLATION!");
                current_state = 5;
            }
        }
        else if (distance_passenger > 0 && distance_passenger < p_warning_limit) {
            barrier_close();
            reset_indicators();
            yellow_led_on();
            if (current_state != 2) {
                lcd_command(0x01); lcd_set_cursor(0,0); lcd_string("WARNING!");
                lcd_set_cursor(1,0); lcd_string("DO NOT CROSS");
                current_state = 2;
            }
        }
        else {
            barrier_open();
            reset_indicators();
            green_led_on();
            if (current_state != 1) {
                lcd_command(0x01); lcd_set_cursor(0,0); lcd_string("PLATFORM SECURE");
                current_state = 1;
            }
        }
    }
}