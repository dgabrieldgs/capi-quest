/*
  PASSA OU REPASSA - funciona sozinho (LCD) ou ligado a pagina web (Web Serial)

  Biblioteca: LiquidCrystal_I2C (Frank de Brabander)
  LCD 16x2 I2C: SDA -> A4 | SCL -> A5 | VCC -> 5V | GND -> GND
  Botoes (pino <-> GND, INPUT_PULLUP):
    D2 Equipe A | D3 Equipe B | D4 ACERTOU | D5 ERROU/PASSA
    D6 RESET (segure 1,5 s)
  Buzzer passivo: D8 (~100 ohm em serie) | LED A: D9 | LED B: D10 (220 ohm)

  MODOS
    Sozinho: o Arduino controla o jogo (ate receber algo da pagina).
    Web:     a pagina controla placar e timer; o Arduino envia os botoes
             e mostra o estado no LCD, LEDs e buzzer.

  PROTOCOLO (9600 baud, uma mensagem por linha)
    Recebe: PING, RESET, QUESTION, ARM, TURN:n, PASSA:n, REPASSA:n,
            CORRECT:n, WRONG:n, END, TIME:s, SCORE:a,b, BYE
    Envia:  READY, PONG, BTN:1, BTN:2, PASSA, CERTO, ERRADO, ZERAR
*/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ---------- Configuracoes ----------
const uint8_t  TEMPO_SEGUNDOS = 10;     // usado no modo sozinho
const uint8_t  PONTOS_POR_ACERTO = 1;
const uint16_t RESET_MS = 1500;
const uint8_t  LCD_ENDERECO = 0x27;     // se nao funcionar, tente 0x3F
const uint16_t PASSO_MS = 150;          // velocidade da capivara (menor = mais rapida)
const uint16_t PAUSA_MS = 800;          // texto parado depois que ela sai
// 20 passos: entra pela esquerda (-4) e sai pela direita (16)
const uint16_t CAPIVARA_MS = 20 * PASSO_MS + PAUSA_MS;

// ---------- Pinos ----------
const uint8_t PIN_A = 2, PIN_B = 3, PIN_ACERTO = 4, PIN_ERRO = 5, PIN_RESET = 6;
const uint8_t PIN_BUZZER = 8, PIN_LED_A = 9, PIN_LED_B = 10;

LiquidCrystal_I2C lcd(LCD_ENDERECO, 16, 2);

// Capivara em 8 caracteres personalizados (4 colunas x 2 linhas)
byte CAPIVARA[8][8] = {
  {B00000, B00000, B00000, B00000, B00000, B00000, B00011, B00111},
  {B00000, B00000, B00000, B00000, B00001, B00001, B11111, B11111},
  {B00000, B01100, B11111, B11111, B11101, B11111, B11111, B11111},
  {B00000, B00000, B11100, B11110, B11111, B11111, B11111, B11111},
  {B01111, B11111, B11111, B11111, B01111, B00111, B00110, B00111},
  {B11111, B11111, B11111, B11111, B11111, B00001, B00001, B00001},
  {B11111, B11111, B11110, B11100, B11000, B11000, B10000, B11000},
  {B11111, B11111, B00000, B00000, B00000, B00000, B00000, B00000}
};

// Segundo quadro: 3 ultimas linhas dos caracteres 4, 5 e 6 (pernas)
byte PERNAS[3][3] = {
  {B00011, B00001, B00001},
  {B10011, B10001, B11001},
  {B10000, B10000, B11000}
};

// ---------- Estado ----------
enum Estado { ESPERA, JOGANDO };
Estado estado = ESPERA;

int pontosA = 0, pontosB = 0;
int equipeAtual = -1;        // 0 = A, 1 = B, -1 = nenhuma
bool jaRepassou = false;
unsigned long inicioTimer = 0;
int ultimoSegundoBeep = -1;
unsigned long ultimoLcd = 0;

bool modoWeb = false;
bool armado = false;         // modo web: aceitando a primeira buzina
int tempoWeb = 0;            // segundos recebidos da pagina
const char* avisoWeb = "Aguarde pergunta";

bool mostraCapivara = false;
int ultimoPasso = -1;
uint8_t quadroPernas = 0;
unsigned long inicioCapivara = 0;

char linhaSerial[24];
uint8_t tamLinha = 0;

// ---------- Botoes com debounce ----------
struct Botao {
  uint8_t pino;
  bool estavel;              // true = solto
  bool ultimaLeitura;
  unsigned long tMudanca;
};

Botao botoes[5] = {
  {PIN_A, true, true, 0},
  {PIN_B, true, true, 0},
  {PIN_ACERTO, true, true, 0},
  {PIN_ERRO, true, true, 0},
  {PIN_RESET, true, true, 0}
};
enum { BA, BB, BACERTO, BERRO, BRESET };

bool pressionou[5];
unsigned long tResetPress = 0;

void lerBotoes() {
  unsigned long agora = millis();
  for (uint8_t i = 0; i < 5; i++) {
    pressionou[i] = false;
    bool leitura = digitalRead(botoes[i].pino);
    if (leitura != botoes[i].ultimaLeitura) {
      botoes[i].tMudanca = agora;
      botoes[i].ultimaLeitura = leitura;
    }
    if ((agora - botoes[i].tMudanca) > 30 && leitura != botoes[i].estavel) {
      botoes[i].estavel = leitura;
      if (leitura == LOW) pressionou[i] = true;
    }
  }
}

// ---------- Utilitarios ----------
void atualizaLeds() {
  digitalWrite(PIN_LED_A, equipeAtual == 0 ? HIGH : LOW);
  digitalWrite(PIN_LED_B, equipeAtual == 1 ? HIGH : LOW);
}

int segundosRestantes() {
  unsigned long decorrido = millis() - inicioTimer;
  long restante = (long)TEMPO_SEGUNDOS * 1000L - (long)decorrido;
  if (restante < 0) restante = 0;
  return (restante + 999) / 1000;
}

void iniciaVez(int equipe) {
  equipeAtual = equipe;
  estado = JOGANDO;
  inicioTimer = millis();
  ultimoSegundoBeep = -1;
  tone(PIN_BUZZER, 1200, 120);
  atualizaLeds();
}

void voltaParaEspera() {
  mostraCapivara = false;
  estado = ESPERA;
  equipeAtual = -1;
  jaRepassou = false;
  atualizaLeds();
}

void repassaOuEncerra() {
  if (!jaRepassou) {
    jaRepassou = true;
    mostraCapivara = false;
    iniciaVez(equipeAtual == 0 ? 1 : 0);
    tone(PIN_BUZZER, 600, 250);
  } else {
    tone(PIN_BUZZER, 300, 500);
    voltaParaEspera();
  }
}

// escreve uma linha completando com espacos ate 16 colunas
void escreveLinha(uint8_t row, const char* texto) {
  char buf[17];
  snprintf(buf, sizeof(buf), "%-16s", texto);
  lcd.setCursor(0, row);
  lcd.print(buf);
}

// troca o desenho das pernas (0 ou 1) para parecer que ela anda
void poePernas(uint8_t quadro) {
  for (uint8_t c = 0; c < 3; c++) {
    byte desenho[8];
    memcpy(desenho, CAPIVARA[4 + c], 8);
    if (quadro == 1) memcpy(desenho + 5, PERNAS[c], 3);
    lcd.createChar(4 + c, desenho);
  }
  quadroPernas = quadro;
}

// chamada quando uma equipe aperta primeiro
void chamaCapivara() {
  mostraCapivara = true;
  inicioCapivara = millis();
  ultimoPasso = -1;
  ultimoLcd = millis() - 100;  // desenha o LCD imediatamente
}

// A capivara atravessa a tela e vai revelando quem apertou
void desenhaCapivara() {
  int passo = (millis() - inicioCapivara) / PASSO_MS;
  if (passo > 20) passo = 20;
  int posicao = passo - 4;     // coluna da traseira da capivara

  // depois que ela sai, o timer continua sendo atualizado
  if (passo == ultimoPasso && passo < 20) return;
  ultimoPasso = passo;

  uint8_t quadro = (passo < 20) ? passo % 2 : 0;
  if (quadro != quadroPernas) poePernas(quadro);

  char texto[2][17];
  int seg = modoWeb ? tempoWeb : segundosRestantes();
  snprintf(texto[0], 17, "   EQUIPE %c!    ", equipeAtual == 0 ? 'A' : 'B');
  if (seg > 0) snprintf(texto[1], 17, " PRIMEIRO! T:%02d ", seg);
  else         snprintf(texto[1], 17, "   PRIMEIRO!    ");

  for (uint8_t row = 0; row < 2; row++) {
    lcd.setCursor(0, row);
    for (int col = 0; col < 16; col++) {
      if (col >= posicao && col < posicao + 4)
        lcd.write((uint8_t)(row * 4 + col - posicao));  // capivara
      else if (col < posicao)
        lcd.write((uint8_t)texto[row][col]);            // por onde ja passou
      else
        lcd.write((uint8_t)' ');                        // ainda nao chegou
    }
  }
}

void desenhaLcd() {
  if (mostraCapivara && millis() - inicioCapivara >= CAPIVARA_MS) mostraCapivara = false;
  if (mostraCapivara && estado == JOGANDO) {
    desenhaCapivara();
    return;
  }

  char linha[17];
  snprintf(linha, sizeof(linha), "A:%02d  B:%02d", pontosA, pontosB);
  escreveLinha(0, linha);

  if (estado == JOGANDO) {
    int seg = modoWeb ? tempoWeb : segundosRestantes();
    char letra = equipeAtual == 0 ? 'A' : 'B';
    const char* rep = jaRepassou ? " (rep)" : "      ";
    // um espaco antes de T: para caber nas 16 colunas
    if (modoWeb && seg == 0) snprintf(linha, sizeof(linha), "Vez %c%s", letra, rep);
    else snprintf(linha, sizeof(linha), "Vez %c%s T:%02d", letra, rep, seg);
    escreveLinha(1, linha);
  } else {
    escreveLinha(1, modoWeb ? avisoWeb : "Aperte p/ jogar ");
  }
}

void animacaoReset() {
  lcd.clear();
  lcd.print("Placar zerado!");
  tone(PIN_BUZZER, 800, 150);
  delay(200);
  tone(PIN_BUZZER, 500, 150);
  delay(800);
  lcd.clear();
}

// ---------- Comandos vindos da pagina ----------
void paraRodadaWeb(const char* aviso) {
  voltaParaEspera();
  armado = false;
  tempoWeb = 0;
  avisoWeb = aviso;
}

void vezWeb(int canal, bool repassada) {
  armado = false;
  estado = JOGANDO;
  equipeAtual = (canal == 1 || canal == 2) ? canal - 1 : -1;
  if (equipeAtual < 0) { estado = ESPERA; avisoWeb = "Outro jogador"; }
  jaRepassou = repassada;
  if (repassada || equipeAtual < 0) mostraCapivara = false;
  tempoWeb = 0;
  atualizaLeds();
}

void trataComando(char* cmd) {
  char* arg = strchr(cmd, ':');
  if (arg) *arg++ = '\0';
  int n = arg ? atoi(arg) : 0;

  if (strcmp(cmd, "BYE") == 0) {          // volta ao modo sozinho
    modoWeb = false;
    pontosA = pontosB = 0;
    voltaParaEspera();
    return;
  }
  if (!modoWeb) {                         // primeira mensagem da pagina
    modoWeb = true;
    paraRodadaWeb("Aguarde pergunta");
  }

  if (strcmp(cmd, "PING") == 0) {
    Serial.println(F("PONG"));
  } else if (strcmp(cmd, "RESET") == 0) {
    paraRodadaWeb("Aguarde pergunta");
  } else if (strcmp(cmd, "QUESTION") == 0 || strcmp(cmd, "ARM") == 0) {
    paraRodadaWeb("Aperte p/ jogar ");
    armado = true;
    if (cmd[0] == 'Q') tone(PIN_BUZZER, 1000, 150);
  } else if (strcmp(cmd, "TURN") == 0) {
    vezWeb(n, false);
    tone(PIN_BUZZER, 1200, 120);
  } else if (strcmp(cmd, "PASSA") == 0 || strcmp(cmd, "REPASSA") == 0) {
    vezWeb(n, true);
    tone(PIN_BUZZER, 600, 250);
  } else if (strcmp(cmd, "CORRECT") == 0) {
    paraRodadaWeb("ACERTOU!");
    tone(PIN_BUZZER, 1500, 300);
  } else if (strcmp(cmd, "WRONG") == 0) {
    paraRodadaWeb("ERROU!");
    tone(PIN_BUZZER, 300, 500);
  } else if (strcmp(cmd, "END") == 0) {
    paraRodadaWeb("Fim de jogo!");
    tone(PIN_BUZZER, 880, 800);
  } else if (strcmp(cmd, "TIME") == 0) {
    tempoWeb = n;
    if (n > 0 && n <= 3) tone(PIN_BUZZER, 900, 80);
  } else if (strcmp(cmd, "SCORE") == 0 && arg) {
    pontosA = atoi(arg);
    char* virgula = strchr(arg, ',');
    if (virgula) pontosB = atoi(virgula + 1);
  }
}

void lerSerial() {
  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\n') {
      linhaSerial[tamLinha] = '\0';
      if (tamLinha > 0) trataComando(linhaSerial);
      tamLinha = 0;
    } else if (c != '\r' && tamLinha < sizeof(linhaSerial) - 1) {
      linhaSerial[tamLinha++] = c;
    }
  }
}

// ---------- Botoes em cada modo ----------
void botoesWeb() {
  if (armado && (pressionou[BA] || pressionou[BB])) {
    armado = false;                        // so o primeiro vale
    int equipe = pressionou[BA] ? 0 : 1;
    equipeAtual = equipe;                  // confirma na hora no LED
    atualizaLeds();
    estado = JOGANDO;
    tempoWeb = 0;                          // a pagina envia o tempo em seguida
    chamaCapivara();
    Serial.println(equipe == 0 ? F("BTN:1") : F("BTN:2"));
  }
  if (pressionou[BACERTO]) Serial.println(F("CERTO"));
  // mesma regra do modo sozinho: 1o erro passa, 2o erro encerra
  if (pressionou[BERRO]) Serial.println(jaRepassou ? F("ERRADO") : F("PASSA"));
}

void botoesSozinho() {
  if (estado == ESPERA) {
    if (pressionou[BA])      { iniciaVez(0); chamaCapivara(); }
    else if (pressionou[BB]) { iniciaVez(1); chamaCapivara(); }
    return;
  }
  if (pressionou[BACERTO]) {
    if (equipeAtual == 0) pontosA += PONTOS_POR_ACERTO;
    else                  pontosB += PONTOS_POR_ACERTO;
    tone(PIN_BUZZER, 1500, 300);
    voltaParaEspera();
  } else if (pressionou[BERRO]) {
    repassaOuEncerra();
  } else {
    int seg = segundosRestantes();
    if (seg <= 3 && seg > 0 && seg != ultimoSegundoBeep) {
      ultimoSegundoBeep = seg;
      tone(PIN_BUZZER, 900, 80);
    }
    if (seg == 0) repassaOuEncerra();
  }
}

// ---------- Setup / Loop ----------
void setup() {
  Serial.begin(9600);
  for (uint8_t i = 0; i < 5; i++) pinMode(botoes[i].pino, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LED_A, OUTPUT);
  pinMode(PIN_LED_B, OUTPUT);

  lcd.init();
  lcd.backlight();
  for (uint8_t i = 0; i < 8; i++) lcd.createChar(i, CAPIVARA[i]);
  escreveLinha(0, "PASSA OU REPASSA");
  escreveLinha(1, "   Boa sorte!");
  tone(PIN_BUZZER, 1000, 150);
  delay(1500);
  lcd.clear();
  atualizaLeds();
  Serial.println(F("READY"));
}

void loop() {
  lerSerial();
  lerBotoes();

  // RESET: segurar para zerar
  if (pressionou[BRESET]) tResetPress = millis();
  if (botoes[BRESET].estavel == LOW && tResetPress != 0 &&
      (millis() - tResetPress) >= RESET_MS) {
    tResetPress = 0;
    if (modoWeb) {
      Serial.println(F("ZERAR"));       // a pagina zera e devolve o placar
    } else {
      pontosA = pontosB = 0;
      voltaParaEspera();
    }
    animacaoReset();
  }
  if (botoes[BRESET].estavel == HIGH) tResetPress = 0;

  if (modoWeb) botoesWeb();
  else         botoesSozinho();

  if (millis() - ultimoLcd >= 100) {
    ultimoLcd = millis();
    desenhaLcd();
  }
}
