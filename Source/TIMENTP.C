/****************************************************************************
 *                                                                          *
 * Time synchronization with NTP servers                                    *
 * Copyright (C) 2023 Andrey Hlus                                           *
 *                                                                          *
 ****************************************************************************/

#include <windows.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <winsock.h>

#include <time.h>


typedef struct hname_t {
    char            name[32];
    struct hname_t *next;
} HOSTNAME;



/*
  список адресов NTP
*/
HOSTNAME *hostList = NULL;

/*
  задержка на старте и на выходе, в мсек
*/
unsigned int startWait = 0;
unsigned int finishWait = 0;

/*
  при какой разнице происходит корректировка времени
  по умолчанию = 2 min
*/
int diffTime = 60*2;



void wsa_error(char *func)
{
    printf("    - error: %s failed with error %d\n", func, WSAGetLastError());
}


/*
  удаляет из ip-адреса незначащие нули
*/
int normalize_ip4(char *str, char *out)
{
    char buf[4096];
    int len, cnt;
    char *pos, *res;

    strcpy(buf, str);
    pos = strtok(buf, ".");
    res = out;
    *res = 0;
    cnt = 0;
    while (pos)
    {
        len = strlen(pos);
        if (len < 1 || len > 3)
            return 0;
        while (len > 1)
        {
            if (*pos != '0')
                break;
            pos++;
            len--;
        }
        strcat(res, pos);
        cnt++;
        if (cnt > 4)
            return 0;
        if (cnt < 4)
            strcat(res, ".");
        // к следующему номеру
        pos = strtok(NULL, ".");
    }
    return -1;
}


/*
  проверка строки на соответствие IP-адресу
*/
int validate_ip4(char *s)
{
    char tail[16];              // сюда скинется всё лишнее
    char norm[16];              // номализованный адрес
    char tmp[16];
    unsigned int digit[4];
    int cnt;

    // проверяем длину строки
    cnt = strlen(s);
    if (cnt < 7 || cnt > 15)
        return 0;

    // убираем возможные незначащие нули в адресе
    if (normalize_ip4(s, &norm) == 0)
        return 0;
    // раскладываем строку на отдельные числа
    tail[0] = 0;
    cnt = sscanf(norm, "%3u.%3u.%3u.%3u%s", &digit[0], &digit[1], &digit[2], &digit[3], tail);
    if (cnt != 4 || tail[0]) // в tail[] будет то, что не вписалось в трафарет
        return 0;
    // проверяем на допустимый диапазон [0..255]
    for (cnt = 0; cnt < 4; cnt++)
        if (digit[cnt] > 255)
            return 0;
    // собираем строку обратно
    snprintf(tmp, 16, "%u.%u.%u.%u", digit[0], digit[1], digit[2], digit[3]);
    // если в ней были символы, кроме цифр, то собранный образец
    // не совпадет с начальным
    if (strcmp(norm, tmp) != 0)
        return 0;

    // сохраняем результат
    strcpy(s, norm);
    return 1;
}



/*
  подгружает настройки и адреса серверов NTP
  из TIMENTP.INI
*/
void load_config(void)
{
    char szIniPath[MAX_PATH+1];
    char szKeys[4096];
    char szHostName[32];
    char szAddress[32];
    char szNum[16];
    char *pos;
    DWORD res;
    HOSTNAME *curIP;

    // сначала инициализируем список адресов дефолтным значением
    hostList = malloc(sizeof(HOSTNAME));
    strcpy(&hostList->name, "200.20.186.76");
    hostList->next = NULL;

    // получем путь к INI-файлу
    GetModuleFileName(NULL, szIniPath, sizeof(szIniPath));
    pos = strrchr(szIniPath, '\\');
    if (*pos == '\\')
    {
        pos++;
        *pos = '\0';
    }
    else {
        strcpy(szIniPath, ".\\");
    }
    strcat(szIniPath, "timentp.ini");

    /*
      считываем время задержек
    */
    res = GetPrivateProfileString("DELAY", "StartWait", "", &szKeys, sizeof(szKeys), szIniPath);
    if (res)
    {
        if (strlen(szKeys) > 0)
            startWait = atoi(szKeys);
        if (startWait > 3600000)
            startWait = 3600000;
    }
    res = GetPrivateProfileString("DELAY", "FinishWait", "", &szKeys, sizeof(szKeys), szIniPath);
    if (res)
    {
        if (strlen(szKeys) > 0)
            finishWait = atoi(szKeys);
        if (finishWait > 3600000)
            finishWait = 3600000;
    }

    /*
      считываем управляющие ключи
    */
    res = GetPrivateProfileString("COMMAND", "DiffTime", "", &szKeys, sizeof(szKeys), szIniPath);
    if (res)
    {
        if (strlen(szKeys) > 0)
            diffTime = atoi(szKeys);
    }
    /*
      получаем список всех ключей в разделе [HOST]
    */
    memset(szKeys, 0, sizeof(szKeys));
    res = GetPrivateProfileString("HOST", NULL, "", &szKeys, sizeof(szKeys), szIniPath);
    if (res == 0)
        return;
    /*
      нашли что-то, и теперь проходимся по найденным ключам
    */
    pos = &szKeys;
    curIP = hostList;
    while (*pos != 0)
    {
        if (GetPrivateProfileString("HOST", pos, "", &szHostName, sizeof(szHostName), szIniPath))
        {
            if (strlen(szHostName) > 0)
            {
                // ключ не пустой, добавляем адрес хоста
                if (validate_ip4(szHostName) > 0)
                {
                    // адрес валидный, добавляем
                    // printf("    - add host at %s\n", szHostName);
                    curIP->next = malloc(sizeof(HOSTNAME));
                    curIP = curIP->next;
                    strcpy(&curIP->name, szHostName);
                    curIP->next = NULL;
                }
            }
        }
        pos = pos + strlen(pos) + 1;
    }
}




int settime(time_t *newtime)
{
    struct tm *ntime;
    SYSTEMTIME time;
    int result;

    ntime = localtime (newtime);

    time.wYear      = ntime->tm_year + 1900;
    time.wMonth     = ntime->tm_mon+1;
    time.wDayOfWeek = ntime->tm_wday;
    time.wDay       = ntime->tm_mday;
    time.wHour      = ntime->tm_hour;
    time.wMinute    = ntime->tm_min;
    time.wSecond    = ntime->tm_sec;
    time.wMilliseconds = 0;

    result = SetLocalTime(&time);
    if (result)
    {
        printf("done!\n");
    } else {
        printf("\n    - error: cat't update time\n");
    }
    return result;
}



/*
  запрашивает на hostname текущее время
  в случае ошибки возвращает ноль,
  иначе время в секундах от 1 января 1900 года
*/
time_t ntp_gettime(char *hostname)
{
    int ntpPort=123;             // порт NTP сервера
    DWORD waitRequest = 6000;    // макс. задержка ожидания ответа сервера (мсек)
    unsigned char sendPkt[48];   // пакет с запросом к серверу NTP
    unsigned long buf[1024];     // буфер для приёма
    int sockAdrSize;
    struct sockaddr sockAddr;
    struct sockaddr_in servAddr; // адрес сервера
    SOCKET hSocket;              // сокет
    time_t ntpTime;              // полученное от сервера время в сек

    memset(&sendPkt, 0, sizeof(sendPkt));
    memset(&buf, 0, sizeof(buf));
    sendPkt[0] = 8;

    // открываем UDP сокет
    hSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (hSocket == INVALID_SOCKET)
    {
        //
        wsa_error("socket()");
        return 0;
    }
    // задаём адрес
    memset(&servAddr, 0, sizeof(servAddr));
    servAddr.sin_family=AF_INET;
    servAddr.sin_addr.s_addr = inet_addr(hostname);
    servAddr.sin_port=htons(ntpPort);
    // задаём макс. время ожидания ответа
    if (setsockopt(hSocket, SOL_SOCKET, SO_RCVTIMEO, (const char*) &waitRequest, sizeof(DWORD)) == SOCKET_ERROR)
    {
        //
        wsa_error("setsockopt()");
        closesocket(hSocket);
        return 0;
    }
    // собственно запрос и ответ
    sockAdrSize = sizeof (sockAddr);
    sendto(hSocket, (const char*) sendPkt, sizeof(sendPkt), 0, (struct sockaddr *) &servAddr, sizeof(servAddr));

    if (recvfrom(hSocket, (char*) buf, 48, 0, &sockAddr, &sockAdrSize) == SOCKET_ERROR)
    {
        wsa_error("recvfrom()");
        closesocket(hSocket);
        return 0;
    }
    // получаем время передачи
    ntpTime = (time_t) ntohl(buf[4]);
    // переводим к 1900 году
    ntpTime = (unsigned long) ntpTime - 2208988800U;

    closesocket(hSocket);
    return ntpTime;
}


/*
  синхронизация времени с сервером NTP
  если получилось, то возвращает -1, иначе 0
*/
int ntpupdate(char *hostname)
{
    time_t srcNtpTime;          // полученное время в сек
    time_t srcLocTime;          // локальное время в сек
    struct tm *tmpTime;         // времянка для конвертации времени
    struct tm locTime;
    struct tm ntpTime;

    printf("- request to server %s\n", hostname);

    // получаем время сервера
    srcNtpTime = ntp_gettime(hostname);
    if (srcNtpTime == 0)
    {
        // что-то пошло не так
        return 0;
    }
    // теперь получаем локальное время и переводим в формат TM
    time(&srcLocTime);
    tmpTime = localtime (&srcLocTime);
    memcpy(&locTime, tmpTime, sizeof(locTime));

    tmpTime = localtime (&srcNtpTime);
    memcpy(&ntpTime, tmpTime, sizeof(ntpTime));
    printf("  - ntp   time: %u/%u/%u  %02u:%02u:%02u\n",ntpTime.tm_mday,ntpTime.tm_mon+1,ntpTime.tm_year+1900,ntpTime.tm_hour,ntpTime.tm_min,ntpTime.tm_sec);
    printf("  - local time: %u/%u/%u  %02u:%02u:%02u\n",locTime.tm_mday,locTime.tm_mon+1,locTime.tm_year+1900,locTime.tm_hour,locTime.tm_min,locTime.tm_sec);

    // сравниваем и если что, то корректируем местное время,
    if (diffTime > 0)
    {
        if (abs(srcNtpTime - srcLocTime) >= diffTime)
        {
            printf("  - update time... ");
            srcNtpTime = ntp_gettime(hostname);
            if (srcNtpTime == 0)
                return 0;
            settime(&srcNtpTime);
        }
    }
    return -1;
}



/*
  синхронизация времени со всеми возможными NTP-серверами.
*/
int do_time(void)
{
    HOSTNAME *host;

    host = hostList;

    while (host)
    {
        if (ntpupdate(&host->name) != 0)
            return 0;                       // получилось, уходим
        host = host->next;
    }
    printf("ERROR: can't connect to NTP servers!\n");
    return 1;
}


int main(void)
{
    WSADATA wsaData;
    int res;

    printf("Time synchronization with NTP servers. (c) Andrey Hlus, 2023\n");
    load_config();

    printf("Please wait %u second...\n", startWait / 1000);
    if (startWait > 10)
        Sleep(startWait);
    res = WSAStartup(MAKEWORD(2,2),&wsaData);
    if (res != NO_ERROR)
    {
        printf("Socket startup failed with error %d\n", res);
        return 1;
    }
    res = do_time();

    WSACleanup();
    printf("Good bye!\n");

    if (finishWait > 10)
        Sleep(finishWait);
    return res;
}

