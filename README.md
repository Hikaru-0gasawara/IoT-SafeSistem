# IoT-SafeSistem

Cofre eletrônico baseado em **ESP32**, com display I2C, teclado matricial 3×4 e servo motor SG90.
O projeto inclui uma implementação principal para hardware real (ESP32) e um **port em MicroPython
para Raspberry Pi Pico** acompanhado de um diagrama Wokwi (Arduino Uno) usado como referência do
circuito original de Uri Shaked.

Adaptado de [Uri Shaked — Arduino Electronic Safe (2020, MIT License)](https://github.com/urish/arduino-electronic-safe).

---

## Sumário

- [Funcionalidades](#funcionalidades)
- [Estrutura do repositório](#estrutura-do-repositório)
- [Hardware](#hardware)
- [Alimentação do servo (LEIA ISSO)](#alimentação-do-servo-leia-isso)
- [Ligações](#ligações)
- [Build & flash](#build--flash)
- [Operação](#operação)
- [Configuração](#configuração)
- [Persistência (NVS)](#persistência-nvs)
- [Troubleshooting](#troubleshooting)
- [Versão MicroPython / Wokwi](#versão-micropython--wokwi)
- [Licença](#licença)

---

## Funcionalidades

- Tela de boas-vindas e UI em LCD 16×2 com ícones customizados (cadeado aberto/fechado, seta).
- Definição e confirmação de senha de 4 dígitos via teclado matricial.
- Servo se move suavemente entre as posições **LOCK** e **UNLOCK** e em seguida desliga o PWM
  para eliminar o jitter do SG90 — a tranca mecânica segura a posição sozinha.
- Estado (trancado/destrancado) e senha persistidos na flash via **Preferences (NVS)**, sobrevivem
  a reset/power-off.
- Recuperação defensiva caso a chave de senha seja apagada parcialmente no NVS.
- Polling do teclado com `delay(KEY_POLL_MS)` — não dispara o watchdog do ESP32.

---

## Estrutura do repositório

```
IoT-SafeSistem/
├── IoT-SafeSistem.ino   # Sketch principal (ESP32 + LCD I2C + keypad + servo)
├── README.md
└── wokwi/
    ├── diagram.json     # Diagrama Wokwi do circuito original (Arduino Uno)
    └── main.py          # Port MicroPython para Raspberry Pi Pico
```

> A pasta `wokwi/` contém o material de referência. O sketch da **raiz**
> (`IoT-SafeSistem.ino`) é o firmware real do projeto.

---

## Hardware

| Componente              | Quantidade | Observações                                     |
|-------------------------|-----------|-------------------------------------------------|
| ESP32 DevKit v1         | 1         | Qualquer placa ESP32 com pinos suficientes      |
| Módulo LCD 16×2 + PCF8574 (I2C) | 1 | Endereços comuns: `0x27` ou `0x3F`              |
| Teclado matricial 3×4   | 1         | Membrana ou rígido                              |
| Servo SG90              | 1         | **Não alimentar pelo ESP32** (ver abaixo)       |
| Fonte 5 V externa ≥ 1 A | 1         | Para o servo                                    |
| Jumpers, protoboard     | —         |                                                 |

### Bibliotecas Arduino (Library Manager)

- `LiquidCrystal_I2C` — qualquer fork compatível com `begin(cols, rows)`.
- `Keypad` (Mark Stanley / Alexander Brevig).
- `ESP32Servo` (Kevin Harrington).

Selecione a placa: **Tools → Board → ESP32 Dev Module**.

---

## Alimentação do servo (LEIA ISSO)

> **Não alimente o SG90 pelo 5V/VIN do ESP32 em hardware real.**
> O pico de corrente do servo (~600 mA em stall) causa brown-out e reinicia o ESP32.

```
   Fonte 5 V (+) ─────────► Servo V+   (vermelho)
   Fonte 5 V (−) ─┬───────► Servo GND  (marrom)
                  └───────► ESP32 GND   (referencial comum)
   ESP32 GPIO 18 ─────────► Servo PWM  (laranja)
```

A fonte 5 V externa **deve compartilhar o GND com o ESP32**, caso contrário o sinal PWM não tem
referência.

---

## Ligações

### LCD I2C

| LCD | ESP32     |
|-----|-----------|
| VCC | 5 V       |
| GND | GND       |
| SDA | GPIO 21   |
| SCL | GPIO 22   |

### Teclado 3×4

| Pino do teclado | ESP32     |
|-----------------|-----------|
| R1              | GPIO 13   |
| R2              | GPIO 14   |
| R3              | GPIO 27   |
| R4              | GPIO 26   |
| C1              | GPIO 25   |
| C2              | GPIO 33   |
| C3              | GPIO 32   |

### Servo SG90

| Servo            | Onde liga                |
|------------------|--------------------------|
| V+ (vermelho)    | Fonte 5 V externa (+)    |
| GND (marrom)     | Fonte 5 V externa (−) e GND do ESP32 |
| PWM (laranja)    | GPIO 18 do ESP32         |

---

## Build & flash

1. Abra `IoT-SafeSistem.ino` no Arduino IDE.
2. Selecione a placa **ESP32 Dev Module** e a porta serial correta.
3. Instale as três bibliotecas listadas em *Hardware*.
4. Compile e faça upload.
5. Abra o **Serial Monitor a 115200 baud** para acompanhar os eventos de servo.

---

## Operação

Ao ligar, o cofre exibe a tela de boas-vindas e em seguida entra no estado persistido (trancado
ou destrancado).

### Estado destrancado

```
[🔓] # to lock      [🔓]
   * = new code
```

- **`#`** — tranca o cofre (move o servo para `SERVO_LOCK_POS` e persiste o estado).
- **`*`** — define ou troca a senha (apenas se já existir uma senha cadastrada). Se nenhum
  código existir ainda, o sistema **força** o cadastro de uma nova senha antes de trancar.

### Estado trancado

```
[🔒] Safe Locked! [🔒]
       [____]
```

- Digite os 4 dígitos. Se a senha conferir, o servo move para `SERVO_UNLOCK_POS` e o cofre passa
  para o estado destrancado. Caso contrário, exibe **Access Denied!** e mantém-se trancado.

### Mensagens

| Mensagem        | Significado                                                 |
|-----------------|-------------------------------------------------------------|
| Welcome!        | Inicialização (mostrada a cada boot).                       |
| Enter new code  | Aguarda 4 dígitos para definir a senha.                     |
| Confirm new code| Aguarda 4 dígitos para confirmar a senha.                   |
| Code mismatch   | As duas senhas digitadas diferem; cofre permanece destrancado. |
| Access Denied!  | Senha incorreta no estado trancado.                         |
| Unlocked!       | Senha correta; cofre abriu.                                 |

---

## Configuração

Todas as constantes ficam no topo do `IoT-SafeSistem.ino`:

| Constante                | Padrão | Descrição                                              |
|--------------------------|--------|--------------------------------------------------------|
| `SERVO_PIN`              | 18     | GPIO do PWM do servo.                                  |
| `SERVO_LOCK_POS`         | 0      | Ângulo de fechamento. Inverta com `UNLOCK_POS` se a mecânica abrir ao contrário. |
| `SERVO_UNLOCK_POS`       | 90     | Ângulo de abertura.                                    |
| `SERVO_STEP_DELAY_MS`    | 8      | ms por grau no movimento suave (menor = mais rápido).  |
| `SERVO_SETTLE_DELAY_MS`  | 150    | Tempo de acomodação antes de `detach()`.               |
| `SERVO_PULSE_MIN_US`     | 500    | Largura mínima do pulso PWM.                           |
| `SERVO_PULSE_MAX_US`     | 2400   | Largura máxima do pulso PWM.                           |
| `LCD_ADDR`               | `0x27` | Endereço I2C do PCF8574. Use `0x3F` se necessário.     |
| `CODE_LENGTH`            | 4      | Tamanho da senha (a UI atual assume 4 dígitos).        |
| `KEY_LOCK` / `KEY_NEW_CODE` | `#` / `*` | Teclas de ação no estado destrancado.            |
| `KEY_POLL_MS`            | 10     | Intervalo entre leituras do teclado (evita watchdog).  |

---

## Persistência (NVS)

A classe `SafeState` encapsula o objeto `Preferences` (NVS interna da ESP32):

| Namespace | Chave    | Tipo   | Conteúdo                          |
|-----------|----------|--------|-----------------------------------|
| `safe`    | `locked` | bool   | Estado físico (trancado/aberto)   |
| `safe`    | `code`   | string | Senha em **texto puro**           |

> ⚠️ A senha é gravada em texto puro na NVS — adequado para um projeto educacional/demo, mas
> **não** use este firmware como segurança real sem antes substituir por um hash com salt
> (ex.: `mbedtls_sha256`).

### Apagar a senha / "factory reset"

Para esquecer a senha, basta apagar a NVS. No Arduino IDE, com a placa conectada, rode o
exemplo `Examples → Preferences → StartCounter` e adicione uma vez:

```cpp
Preferences p;
p.begin("safe", false);
p.clear();
p.end();
```

Ou apague o particionamento NVS via `esptool.py erase_flash`.

---

## Troubleshooting

| Sintoma                                | Causa provável / solução                                            |
|----------------------------------------|---------------------------------------------------------------------|
| LCD em branco ou com blocos pretos     | Ajuste o trimpot de contraste no PCF8574. Verifique `LCD_ADDR` — descomente o scanner I2C no final do `.ino`. |
| ESP32 reinicia ao acionar o servo      | Servo alimentado pelo 5V do ESP32. Use fonte externa 5 V ≥ 1 A e una os GNDs. |
| Servo "zumbe" quando parado            | Esperado em SG90; o sketch já chama `detach()` após o movimento.    |
| Teclas não funcionam                   | Confira a ordem de R1..R4 e C1..C3. Use Serial para imprimir `keypad.getKey()` em debug. |
| Senha não muda                         | Verifique se o LCD mostrou "Code mismatch" — as duas confirmações precisam ser idênticas. |
| Watchdog reset (`Task watchdog got triggered`) | Algum `while` sem `delay`. Confirme que está usando `waitForKey()` / `waitForKeyIn()`. |

### Scanner I2C

Cole no `setup()` (já está comentado no final do `.ino`):

```cpp
Wire.begin();
delay(100);
for (byte a = 1; a < 127; a++) {
  Wire.beginTransmission(a);
  if (Wire.endTransmission() == 0) {
    Serial.printf("I2C device at 0x%02X\n", a);
  }
}
```

---

## Versão MicroPython / Wokwi

A pasta `wokwi/` traz:

- **`diagram.json`** — circuito original em **Arduino Uno** (LCD paralelo HD44780, keypad 4×4,
  servo, resistor 220 Ω no backlight). Serve como referência visual do projeto base.
- **`main.py`** — port MicroPython para **Raspberry Pi Pico**, com classes próprias de
  `LCD1602`, `Keypad4x4`, `Servo` e `SafeState` (persistência em arquivo `safe.dat`).

> O diagrama é para Uno e o `main.py` é para Pico — eles **não** rodam juntos no Wokwi.
> Para simular o port MicroPython, troque a parte `wokwi-arduino-uno` por `wokwi-pi-pico` e
> ajuste os pinos das colunas do keypad de `A0..A3` para `GP13..GP16`.

### Diferenças entre o sketch principal e o port MicroPython

| Item       | `IoT-SafeSistem.ino` (raiz) | `wokwi/main.py`              |
|------------|-----------------------------|------------------------------|
| Plataforma | ESP32                       | Raspberry Pi Pico            |
| Linguagem  | C++ / Arduino               | MicroPython                  |
| LCD        | I2C (PCF8574)               | Paralelo 4-bit               |
| Keypad     | 3×4                         | 4×4                          |
| Persistência | NVS (`Preferences`)       | Arquivo `safe.dat` na flash  |
| Servo      | `ESP32Servo` + suavização   | `machine.PWM` 50 Hz          |

---

## Licença

Adaptado de [Uri Shaked — Arduino Electronic Safe](https://github.com/urish/arduino-electronic-safe)
sob licença **MIT**.
