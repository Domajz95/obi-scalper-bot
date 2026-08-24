# OBI Scalper Bot pro Binance

Tento repozitář obsahuje C++ aplikaci představující **OBI (Order Book Imbalance) Scalper bota** určeného pro kryptoměnovou burzu Binance.

Bot analyzuje hloubku knihy objednávek (Order Book) v reálném čase, vypočítává nevyváženost (Imbalance) mezi nabídkou (Asks) a poptávkou (Bids) a na základě definovaných prahových hodnot provádí scalpingové obchody.

## Hlavní funkce

*   **WebSocket Data v reálném čase:** Bot se připojuje k Binance WebSocket API (Multi-stream) a odebírá data o hloubce trhu pro následující měnové páry (úroveň hloubky 10, aktualizace každých 100 ms):
    *   SOLUSDC
    *   SUIUSDC
    *   BTCUSDC
    *   ETHUSDC
*   **Analýza Order Book Imbalance (OBI):** Z přijatých dat se počítá surové a vyhlazené OBI pomocí struktury "RingBuffer", čímž se získává plynulejší signál pro vstup a výstup.
*   **REST API pro obchodování:** Obchodní příkazy (BUY / SELL) jsou bezpečně odesílány přes Binance REST API. Bot si sám synchronizuje čas s Binance a podepisuje požadavky pomocí HMAC-SHA256 s využitím zadaného API klíče a Secret klíče.
*   **Řízení rizik (Risk Management):**
    *   Nastavitelný Target Profit v USD.
    *   Nastavitelný Stop Loss v procentech.
    *   Podmíněný výstup při náhlém poklesu OBI (tzv. "OBI Drop").
    *   Dynamické zjišťování dostupného zůstatku v peněžence před provedením obchodu (REST kontrola zůstatku).
*   **Logování:** Každý úspěšný výstup (uzavření pozice) se ukládá do lokálního souboru `real_trades.csv` společně s časem, vstupní i výstupní cenou, velikostí a realizovaným ziskem/ztrátou (PnL) a důvodem ukončení obchodu.
*   **Vláknová bezpečnost (Thread-safety):** Zpracování WebSocket dat a hlavní obchodní smyčka běží ve vlastních vláknech. Přístup ke sdíleným bufferům s tržními daty a HTTP požadavkům je chráněn pomocí mutexů.

## Požadavky na sestavení

Pro kompilaci a spuštění tohoto bota budete potřebovat kompilátor s podporou **C++17** a následující knihovny:
*   **Boost** (zejména komponenty `system` a `thread`, dále bot využívá `Boost.Beast` a `Boost.Asio`)
*   **OpenSSL** (pro HMAC-SHA256 podepisování a HTTPS/WSS spojení)
*   **nlohmann-json** (pro parsování JSON odpovědí) - tato knihovna je typicky hlavičková (header-only).

V systémech založených na Debian/Ubuntu můžete potřebné závislosti nainstalovat takto:
```bash
sudo apt-get update
sudo apt-get install build-essential cmake libboost-all-dev libssl-dev nlohmann-json3-dev
```

## Jak sestavit projekt (CMake)

Projekt obsahuje standardní `CMakeLists.txt`. Pro zkompilování aplikace postupujte podle těchto kroků:

1.  Vytvořte a přejděte do složky pro build (typicky pojmenované `build`):
    ```bash
    mkdir build
    cd build
    ```
2.  Vygenerujte soubory pro build pomocí CMake:
    ```bash
    cmake ..
    ```
3.  Zkompilujte projekt:
    ```bash
    make
    ```
4.  Po úspěšném dokončení najdete ve složce `build` spustitelný soubor s názvem `OBIScalper`.

## Konfigurace a spuštění

Než bota spustíte, je nutné otevřít soubor `main.cpp` a na začátku funkce `main()` nahradit placeholderové texty za vaše skutečné Binance API klíče:

```cpp
std::string API_KEY = "VÁŠ_BINANCE_API_KLÍČ";
std::string SECRET_KEY = "VÁŠ_BINANCE_SECRET_KLÍČ";
```

*(Upozornění: Pro reálné nasazení doporučujeme klíče načítat z proměnných prostředí nebo bezpečně zašifrovaných konfiguračních souborů a **nikdy** je nenahrávat do veřejného verzovacího systému).*

Po doplnění klíčů a zkompilování jednoduše spusťte bota:
```bash
./OBIScalper
```

Bot se následně připojí k Binance, začne vypisovat do konzole synchronizační zprávy, a pokud objeví tržní nerovnováhu podle nastavených parametrů, zahájí obchodování.

---
**Zřeknutí se odpovědnosti (Disclaimer):** Tento kód slouží pouze pro vzdělávací účely. Obchodování s kryptoměnami nese vysoké riziko. Autor tohoto repozitáře nenese žádnou zodpovědnost za případné finanční ztráty způsobené používáním tohoto bota. Před použitím bota s reálnými finančními prostředky si pečlivě otestujte jeho chování, ideálně na Testnetu nebo s velmi malými částkami.
