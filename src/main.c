#include <avr/interrupt.h>
#include <avr/io.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <util/delay.h>
#include <avr/eeprom.h>

/******************** Настройки ********************/

#define LCD_PORT		PORTB
#define LCD_PIN_RS		PB4
#define LCD_PIN_E		PB5
#define LOCK_PORT		PORTC
#define LOCK_PIN		PC0

#define PASS_LENGTH		6
#define PASS_DEFAULT	"000000"

// Возможные состояния устройства
enum state_e{
	state_locked, // Заблокирован
	state_unlocked, // Разблокирован
	state_unseccess, // Неудачно введенн пароль
	state_setPassword, // Установка пароля
	state_passwordUpdated, // Сообщение об установке нового пароля
};

/******************** Глобальные переменные ********************/

// Расположение кнопок
const char but2char[4][3] = {
	{'1', '2', '3'},
	{'4', '5', '6'},
	{'7', '8', '9'},
	{'*', '0', '#'},
};

// Текущее состояние, volatile тк переменная может обновляться в перывании
volatile enum state_e state = state_locked;

char password[PASS_LENGTH + 1]; // Код разблокировки
char enterBuffer[PASS_LENGTH + 1]; // Вводимый код

/******************** Драйвер дисплея ********************/

void lcdLatch(void){
	// ДИСПЛЕЙ В ПРОТЕУСЕ НЕ СООТВЕТСТВУЕТ ДАТАШИТУ - 450 нс НЕДОСТАТОЧНО!
	LCD_PORT |= (1 << LCD_PIN_E); // Установить высокий уровень на тактовом пине шины
	_delay_us(20); // Задержка, что бы дисплей успел считать данные с шины, должно быть больше 450нс
	LCD_PORT &= ~(1 << LCD_PIN_E); // Установить низкий уровень на тактовом пине шины
	_delay_us(20); // Задержка, что бы дисплей успел считать данные с шины, должно быть больше 450нс
}

void lcdWrite(uint8_t byte){
	LCD_PORT = (byte >> 4) | (LCD_PORT & 0xF0); // Записываем старшие 4 байта на шину
	lcdLatch();
	LCD_PORT = (byte & 0x0F) | (LCD_PORT & 0xF0); // Записываем младшие 4 байта на шину
	lcdLatch();
	_delay_us(100); // Выполнение записи может занять до 40мкс, ждем
}

void lcdWriteCmd(uint8_t cmd){
	LCD_PORT &= ~(1 << LCD_PIN_RS); // Дисплей в режим команд (0 на пине RS)
	lcdWrite(cmd); // Запись команды
}

void lcdWriteData(uint8_t data){
	LCD_PORT |= (1 << LCD_PIN_RS); // Дисплей в режим данных
	lcdWrite(data); // Запись данных
}

void lcdClear(void){
	lcdWriteCmd(0x01); // Команда очистки дисплея
	_delay_ms(2); // Очистка занимает до 1.52мс
}

void lcdSetCursor(uint8_t line, uint8_t columm){
	uint8_t position = (line << 6) | (columm); // Установка позиции курсора
	lcdWriteCmd(0x80 | position);
}

void lcdInit(void){
	// Последовательность иницаиализации из даташита
	_delay_ms(20); // Задержка, что бы дисплей успел включиться
	LCD_PORT = 0x03 | (LCD_PORT & 0xF0); // Записываем старшие 4 байта на шину
	lcdLatch();
	_delay_ms(5);
	LCD_PORT = 0x03 | (LCD_PORT & 0xF0); // Записываем старшие 4 байта на шину
	lcdLatch();
	_delay_us(100); // Выполнение записи может занять до 40мкс, ждем
	
	lcdWriteCmd(0x32);
	lcdWriteCmd(0x28);
	lcdWriteCmd(0x0C);
	lcdWriteCmd(0x06);
	
	lcdClear();
}

void lcdPrint(char *string){
	while(*string){ // Пока в строке не кончились символы, выводим
		lcdWriteData(*string);
		string++;
	}
}

/******************** Обработка кнопок, проверка пароля ********************/

void btnPushedISR(char button){	
	if(state == state_unseccess){
		return; // Если ошибка ввода, не обрабатываем нажатия
	}
	if(button == '#'){ // Если решетка, сброс пароля, или блокировка
		state = state_locked;
		memset(enterBuffer, 0, PASS_LENGTH);
		return;
	}
	if(state == state_unlocked && (button == '*')){ // Если разблокированы, можно сменить пароль
		state = state_setPassword;
	}
	
	// Если в режиме набора пароля и нажата цифра
	if(((state == state_locked) || (state == state_setPassword)) && \
		(button >= '0') && (button <= '9')){
		uint8_t enteredLength = strlen(enterBuffer); // Узнаем сколько символов уже введено
		enterBuffer[enteredLength] = button; // Добавляем символ в конец строки
		if((enteredLength+1) == PASS_LENGTH){ // Если пароль введен полностью
			if(state == state_locked){ 			// и замок заблокирован
				if(!strcmp(password, enterBuffer)){ // проверяем правильность введенного пароля
					state = state_unlocked; // Если правильно, разблокировка
				}else{
					state = state_unseccess; // Иначе вывести сообщение об ошибке ввода
				}
			}else if(state == state_setPassword){ // Если замок в режиме смены кода
				memcpy(password, enterBuffer, PASS_LENGTH); // Меняем код
				eeprom_write_block(password, 0x00, PASS_LENGTH); // Сохраняем в EEPROM
				state = state_passwordUpdated; // Выводим сообщение об смене пароля
			}
			memset(enterBuffer, 0, PASS_LENGTH); // Удаляем введенные данные
		}
	}
}

ISR(TIMER0_OVF_vect){
	static char lastButton;
	for(uint8_t row = 0; row < 4; row++){ // Проверяем строки
		PORTD = ~(1 << row) & (PORTD | 0x0F); // Устанавливаем лог. нуль на той строке, которую надо считать
		for(uint8_t collumn = 4; collumn < 7; collumn++){
			char currentButton = but2char[3-row][collumn-4]; // Проверяем, какую кнопку читаем
			if(!(PIND & (1 << collumn))){ // Если какая то кнопка нажата
				if(currentButton != lastButton){ // Проверяем, что она до этого не была нажата
					btnPushedISR(currentButton); // Вызов обработчика нажатия кнопка
					lastButton = currentButton; // Запоминаем, какая кнопка была нажата
				}
			}else{
				if(currentButton == lastButton){ // Если кнопка была нажата, а теперь нет
					lastButton = 0; // Забываем предыдущую нажатую кнопку
				}
			}
		}
	}
}

/******************** Отображения ********************/

int main(void){
	DDRB = 0b00111111; // Выводы для дисплея в режим выхода
	DDRD = 0b00001111; // Выводы строк клавиатуры в режим выхода
	DDRC = 0b00000001; // Вывод базы транзистора в режим выхода 
	PORTD |= 0b01110000; // Подтяжка вводов матричной клавиатуры
	
	// Частота переполнения (опроса клаватуры) - F_CPU/256/256=244,14 Гц (при F_CPU=16МГц)
	TCCR0B = (1 << CS02); // Включение таймера 0 в нормальном режиме, делитель 256
	TIMSK0 = (1 << TOIE0); // Включение прерывание по перепелнонию
	
	// Загрузка пароля из EEPROM память
	eeprom_read_block(password, 0x00, PASS_LENGTH);
	// Проверка содержимого eeprom
	for(uint8_t i = 0; i < PASS_LENGTH; i++){
		// Если код неинициализирован, установка стандартного кода и записть в EEPROM
		if((password[i] < '0') || (password[i] > '9')){
			strcpy(password, PASS_DEFAULT);
			eeprom_write_block(password, 0x00, PASS_LENGTH);
		}
	}
	sei(); // Велючение прерываний
	
	lcdInit(); // Инициализация дисплея
	
	while (1){
		lcdClear(); // Очистка дисплея
		lcdSetCursor(0,0); // Установить курсор в начало
		
		// В зависимости от состояния вывод нжуного меню
		switch (state){
			case state_unlocked:
				LOCK_PORT |= (1 << LOCK_PIN); // Открыть замок
				lcdPrint("UNLOCKED");
				lcdSetCursor(1,0);
				lcdPrint("#-LOCK *-NEW PIN");
				break;
			case state_unseccess:
				lcdPrint("ERROR!");
				lcdSetCursor(1,0);
				lcdPrint("TRY AGAIN!");
				_delay_ms(3000);
				state = state_locked;
				break;
			case state_locked:
				LOCK_PORT &= ~(1 << LOCK_PIN); // Закрыть замок
				lcdPrint("ENTER PIN:");
				lcdSetCursor(1,0);
				lcdPrint(enterBuffer);
				break;
			case state_setPassword:
				lcdPrint("NEW PIN:");
				lcdSetCursor(1,0);
				lcdPrint(enterBuffer);
				break;
			case state_passwordUpdated:
				lcdPrint("NEW PIN SET!");
				lcdSetCursor(1,0);
				lcdPrint("PIN:");
				lcdPrint(password);
				_delay_ms(2000);
				state = state_unlocked;
			default: break;
		}
		_delay_ms(50); // Что бы не обновлять дисплей слишком часто
	}
}
