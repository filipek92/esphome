# ThingsBoard Bridge

Komponenta propojuje ESPHome zařízení s [ThingsBoard](https://thingsboard.io/) platformou přes MQTT.

## Konfigurace

```yaml
thingsboard:
  server: your-thingsboard-server.com
  token: YOUR_DEVICE_ACCESS_TOKEN
  port: 8883  # volitelné, výchozí 8883 (MQTTS)
  auto_telemetry: true  # volitelné, automatické posílání stavů entit
  attribute_globals:  # volitelné, globals proměnné odeslané jako device attributes
    - uptime_counter
    - firmware_channel
  log_level: WARN  # volitelné, přeposílání logů do ThingsBoard
```

| Klíč        | Povinný | Výchozí | Popis                                    |
|-------------|---------|---------|------------------------------------------|
| `server`    | ano     | —       | Adresa ThingsBoard serveru               |
| `token`     | ano     | —       | Access Token zařízení z ThingsBoard      |
| `port`      | ne      | `8883`  | Port MQTT brokeru (8883 = TLS, 1883 = bez TLS) |
| `auto_telemetry` | ne | `true` | Automatické odesílání telemetrie z entit |
| `attribute_globals` | ne | `[]` | Seznam `globals` ID, které se publikují jako device attributes |
| `log_level` | ne      | —       | Úroveň logů odesílaných do ThingsBoard  |

## Co komponenta dělá automaticky

### Telemetrie (zařízení → ThingsBoard)

Všechny **neinterní** entity se automaticky odesílají jako telemetrie:

| Typ entity      | Odesílaná hodnota        |
|-----------------|--------------------------|
| `sensor`        | `float` hodnota          |
| `binary_sensor` | `true` / `false`         |
| `number`        | `float` hodnota          |
| `switch`        | `true` / `false`         |
| `select`        | zvolená `string` možnost |
| `light`         | `true` / `false` (stav)  |

Klíč v telemetrii je `object_id` entity (např. `temperature`, `relay_1`).

Pokud nastavíte `auto_telemetry: false`, komponenta automatickou telemetrii neposílá.
To je vhodné, když si filtraci a frekvenci odesílání chcete řídit sami přes automatizace
(`thingsboard.send_telemetry`, `interval`, vlastní podmínky).

### Atributy zařízení

Při prvním připojení se odešlou atributy:
- `device_name` — název zařízení
- `esphome_version` — verze ESPHome
- `build_date` — datum kompilace

Navíc všechny **neinterní** `text_sensor` entity se automaticky odesílají jako device attributes (při každé změně hodnoty).

### Atributy z `globals`

Přes `attribute_globals` můžete do ThingsBoard publikovat hodnoty z `globals` jako device attributes.
Každý zadaný `global` se odešle pod stejným klíčem, jako je jeho `id`.

```yaml
globals:
  - id: uptime_counter
    type: int
    initial_value: "0"

  - id: firmware_channel
    type: std::string
    initial_value: '"stable"'

thingsboard:
  server: your-thingsboard-server.com
  token: YOUR_DEVICE_ACCESS_TOKEN
  attribute_globals:
    - uptime_counter
    - firmware_channel
```

Tyto hodnoty se odešlou při úvodním syncu po připojení MQTT (spolu s ostatními device attributes).

## Ovládání z ThingsBoard (RPC)

Komponenta naslouchá na RPC požadavky z ThingsBoard a mapuje je na ESPHome entity.

### Nastavení v ThingsBoard

1. Otevřete **Dashboard** → přidejte widget
2. Ve widgetu nastavte **Target device** na vaše zařízení
3. Použijte akce typu **RPC call** (Server-side nebo Client-side RPC)

### Formát RPC požadavku

ThingsBoard posílá RPC jako JSON na topic `v1/devices/me/rpc/request/{id}`:

```json
{
  "method": "název_metody",
  "params": <hodnota nebo objekt>
}
```

Komponenta podporuje dva formáty:

#### 1. Přímé ovládání jedné entity

`method` = `object_id` entity, `params` = hodnota.

**Switch (zapnout/vypnout):**
```json
{"method": "relay_1", "params": true}
```

**Number (nastavit hodnotu):**
```json
{"method": "target_temperature", "params": 23.5}
```

**Select (vybrat možnost):**
```json
{"method": "fan_mode", "params": "auto"}
```

**Button (stisknout):**
```json
{"method": "restart", "params": null}
```

**Light (zapnout/vypnout):**
```json
{"method": "desk_lamp", "params": true}
```

**Light (nastavit jas):**
```json
{"method": "desk_lamp", "params": {"state": true, "brightness": 128}}
```
> `brightness` je 0–255, komponenta převádí na 0.0–1.0.

#### 2. Hromadné ovládání více entit

`params` je objekt kde klíče jsou `object_id` entit:

```json
{
  "method": "setValues",
  "params": {
    "relay_1": true,
    "relay_2": false,
    "target_temperature": 22.0
  }
}
```

### Odpověď

Komponenta vždy odpoví na RPC:

```json
{"success": true}
```
nebo
```json
{"error": "Unknown method"}
```

### Příklad: Tlačítko ve widgetu

V ThingsBoard dashboardu přidejte **Button widget** s RPC akcí:

- **Method**: `relay_1`
- **Params**: `true` (pro zapnutí) / `false` (pro vypnutí)

Nebo použijte **Switch Control** widget, který automaticky generuje RPC volání.

## Podporované platformy

| Platforma       | Framework | MQTT knihovna        |
|-----------------|-----------|----------------------|
| ESP8266         | Arduino   | PubSubClient         |
| ESP32           | Arduino   | PubSubClient         |
| ESP32           | ESP-IDF   | esp-mqtt (vestavěné) |

## Přeposílání logů

Volitelně lze logy z ESPHome přeposílat jako telemetrii do ThingsBoard. Stačí nastavit `log_level`:

```yaml
thingsboard:
  server: your-thingsboard-server.com
  token: YOUR_DEVICE_ACCESS_TOKEN
  log_level: WARN
```

Dostupné úrovně (od nejméně do nejvíce podrobné):

| Úroveň         | Popis                          |
|-----------------|--------------------------------|
| `NONE`          | Žádné logy (výchozí)           |
| `ERROR`         | Pouze chyby                    |
| `WARN`          | Chyby + varování               |
| `INFO`          | Informační zprávy              |
| `CONFIG`        | Konfigurační zprávy            |
| `DEBUG`         | Debug zprávy                   |
| `VERBOSE`       | Podrobné zprávy                |
| `VERY_VERBOSE`  | Velmi podrobné zprávy          |

Logy se odesílají jako telemetrie s klíčem `log`:

```json
{
  "log": "[W][wifi:123] Connection lost"
}
```

> **Upozornění:** Vysoké úrovně (`DEBUG`, `VERBOSE`) generují velké množství MQTT zpráv. Pro produkční použití doporučujeme `WARN` nebo `ERROR`.

### Dynamická změna úrovně logování

Úroveň logování lze měnit za běhu přes **shared atributy** v ThingsBoard — bez nutnosti překompilovat firmware.

1. V ThingsBoard otevřete **Device** → **Attributes** → záložka **Shared attributes**
2. Přidejte nebo upravte klíč `log_level` s hodnotou např. `"WARN"`, `"DEBUG"` nebo `"NONE"`

Zařízení změnu přijme okamžitě a začne (nebo přestane) odesílat logy podle nové úrovně.

| Hodnota atributu | Efekt                                  |
|------------------|----------------------------------------|
| `NONE`           | Vypne přeposílání logů                 |
| `ERROR`          | Pouze chyby                            |
| `WARN`           | Chyby + varování                       |
| `INFO`           | Informační zprávy a výše               |
| `DEBUG`          | Debug zprávy a výše                    |
| `VERBOSE`        | Podrobné zprávy a výše                 |

> **Tip:** Tímto můžete zapnout debug logování na dálku, diagnostikovat problém a zase ho vypnout — vše bez OTA update.

## Automatizace (Triggery)

Komponenta nabízí dva triggery pro vlastní logiku v YAML automatizacích.

### `on_rpc` — při RPC volání

Spustí se při každém RPC požadavku z ThingsBoard (i pro ty, které komponenta zpracuje interně).

**Proměnné:**
- `method` (`std::string`) — název metody
- `params` (`std::string`) — parametry jako JSON string

```yaml
thingsboard:
  server: your-server.com
  token: YOUR_TOKEN
  on_rpc:
    then:
      - logger.log:
          format: "RPC volání: %s(%s)"
          args: [method.c_str(), params.c_str()]
```

### `on_attribute` — při změně shared atributu

Spustí se pro každý klíč v přijatém shared atributu z ThingsBoard.

**Proměnné:**
- `key` (`std::string`) — název atributu
- `value` (`std::string`) — hodnota jako JSON string

```yaml
thingsboard:
  server: your-server.com
  token: YOUR_TOKEN
  on_attribute:
    then:
      - logger.log:
          format: "Atribut %s = %s"
          args: [key.c_str(), value.c_str()]
```

### Příklad: Vlastní RPC příkaz

```yaml
thingsboard:
  server: your-server.com
  token: YOUR_TOKEN
  on_rpc:
    then:
      - if:
          condition:
            lambda: 'return method == "reboot";'
          then:
            - button.press: restart_button
```

### Vlastní RPC odpověď

V `on_rpc` můžete nastavit vlastní odpověď, kterou ThingsBoard obdrží. Zavolejte `set_rpc_response()` v lambdě:

```yaml
thingsboard:
  id: tb_bridge
  server: your-server.com
  token: YOUR_TOKEN
  on_rpc:
    then:
      - if:
          condition:
            lambda: 'return method == "getStatus";'
          then:
            - lambda: |-
                id(tb_bridge).set_rpc_response("{\"temperature\": 23.5, \"uptime\": 3600}");
```

ThingsBoard pak jako odpověď na RPC obdrží `{"temperature": 23.5, "uptime": 3600}`.

Pokud `set_rpc_response()` nezavoláte, použije se výchozí odpověď:
- `{"success": true}` pokud se entita našla a ovládla
- `{"error": "Unknown method"}` pokud entita nebyla nalezena

## Odesílání dat z automatizací (Akce)

Kromě automatického odesílání entit můžete kdykoli ručně odeslat telemetrii nebo atribut — z YAML automatizací i z C++ lambdy.

### `thingsboard.send_telemetry`

Odešle telemetrii (časová řada) do ThingsBoard.

| Parametr | Povinný | Popis                     |
|----------|---------|---------------------------|
| `key`    | ano     | Klíč telemetrie           |
| `value`  | ano     | Hodnota (string/template) |

```yaml
# Příklad: odeslání vlastní telemetrie při stisku tlačítka
button:
  - platform: template
    name: "Send Status"
    on_press:
      then:
        - thingsboard.send_telemetry:
            key: "custom_metric"
            value: !lambda 'return to_string(id(my_sensor).state);'
```

### `thingsboard.send_attribute`

Odešle device atribut do ThingsBoard.

| Parametr | Povinný | Popis                      |
|----------|---------|----------------------------|
| `key`    | ano     | Klíč atributu              |
| `value`  | ano     | Hodnota (string/template)  |

```yaml
# Příklad: odeslání atributu při startu
esphome:
  on_boot:
    then:
      - thingsboard.send_attribute:
          key: "firmware_variant"
          value: "production"

      - thingsboard.send_attribute:
          key: "room"
          value: !lambda 'return "living_room";'
```

### Použití z C++ lambdy

Pro pokročilé scénáře lze volat metody přímo přes globální pointer `thingsboard::global_tb_bridge`:

```yaml
interval:
  - interval: 60s
    then:
      - lambda: |-
          // Telemetrie
          thingsboard::global_tb_bridge->send_telemetry("free_heap", (float)ESP.getFreeHeap());
          thingsboard::global_tb_bridge->send_telemetry("wifi_rssi", (float)WiFi.RSSI());

          // Atributy (string, float, bool)
          thingsboard::global_tb_bridge->send_attribute("fw_version", "1.2.3");
          thingsboard::global_tb_bridge->send_attribute("uptime_hours", millis() / 3600000.0f);
          thingsboard::global_tb_bridge->send_attribute("ota_enabled", true);
```

Dostupné overloady:

| Metoda              | Typy hodnoty              |
|---------------------|---------------------------|
| `send_telemetry()`  | `float`, `bool`, `string` |
| `send_attribute()`  | `float`, `bool`, `string` |
