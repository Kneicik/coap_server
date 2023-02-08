.. _coap-server-sample:

CoAP Server
###########

Overview
********

Testowy program do serwera CoAP dla drona ROV1.

Program zawiera 2 rodzaje zapytania:
/led metodą PUT można podając na payload 1 albo 0 zapalać i gasić diodę led (pisane z myślą o nucleo-f207zg ale powinno dać się wgrać na każdy inny devboard z ethernetem),
/obs metodą GET z subskrypcją. przykład wysyła zwiększający sie licznik. Dane można wysyłać bardzo szybko i powinno się idealnie nadać do naszego zastosoania.

Najprawdopodobniej używając tych dwóch metod możemy bez problemu sterować ROV1. Da się obserwować parę rzeczy na raz a do tego wysyłać inne zapytania i wygląda na to, że wszystko funkcjonuje jak należy.

Do zrobienia:
- przykład sprawnego klienta do tesów, - gotowe
- komunikacja CAN,                     - gotowe
- sprawdzić czy da się subskrybowac bezterminowo


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