.. _coap-server-sample:

CoAP Server
###########

Overview
********

Testowy program do serwera CoAP dla drona ROV1.

Program zawiera 3 rodzaje zapytania:
/thruster do obsługi silników z tyłu. Należy metodą PUT podać wartość 15 bitową ujemną lub dodatnią aby sterować mocą silników do przodu albo do tyłu.
/level do obsługi silnika góra dół. sterownie jak wyżej.
/obs metodą GET z subskrypcją. przykład wysyła zwiększający sie licznik. Dane można wysyłać bardzo szybko i powinno się idealnie nadać do naszego zastosoania.

Program komunikuje się przez CAN z płytką sterującą silnikami
thrustery są na id 0x010 natomiast level jest na id 0x011
ramka wygląda następująco:
00 00 32 34 - gdzie pierwsze 2 bajty mogą mieć wartość albo same 0 albo F w przypadku gdy liczba jest ujemna a reszta to 15 bitowa wartość do sterowania mocą

Do zrobienia:
- przykład sprawnego klienta do tesów, - gotowe
- komunikacja CAN,                     - gotowe (sprawne obieranie i wysyłanie, poprawić odbieranie czyli odchudzić funkcjei i niepotrzebne funkcje)
- sprawdzić czy da się subskrybowac bezterminowo
- pingowanie klienta


Building And Running
********************

Przykład był pisany z myślą o nucleo-f207zg ale powinien zadziałać na każdym devboardzie obsługującym ethernet lub wifi (możliwa potrzebna dodatkowa konfiguracja w przypadku wifi)

W katalogu głównym zephyra otworzyć terminal następnie wpisać:

source ~/zephyrproject/.venv/bin/activate

west build --pristine -b <nazwa używanej płytki> <ścieżka do przykładu>

west flash

Do zaobserwowania przykładu potrzebny jest klient CoAP w moim przypadku używałem libcoap pod Ubuntu
Proces instalacji:

https://libcoap.net/install.html

w przypadku jetsona, który domyślnie ma starszą wersję systemu wystarczy wpisać w terminal:

sudo apt install libcoap-1-0-bin

Potrzebny jest jeszcze program do komunikacji przez UART do obserwowania zachowania mikrokontrolera. W moim przypadku używałem minicom

minicom -b 115200 -D /dev/ttyACM0      - dla nucleo-f207zg

Przesył pakietów CoAP można śledzić używając wiresharka. Wybrać interfejs siecziowy do którego klient i serwer są połączone a potem dla przejrzystości w filtrowaniu wpisać CoAP

Po spełnieniu powyższych warunków można przetestować działanie wysyłając nastepujące zapytania terminalem:

coap-client -m put -e 1 coap://192.168.2.169/led     

zapytanie służy do zapalania diody led, aby ją zgasić zależy wpisać 0 zamiast 1.

coap-client -m get -s <czas> coap://192.168.2.169/obs  

zapytanie do oberwacji zmieniającej się wartości.

Adres IP był ustwiony pod mój setup ale można to w prosty sposób zmienić w pliku prj.conf