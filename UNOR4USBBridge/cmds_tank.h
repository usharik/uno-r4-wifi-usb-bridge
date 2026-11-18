#ifndef CMDS_TANK_H
#define CMDS_TANK_H

/* Tank extensions to the AT command set (arduino_tank project). Everything here is opt-in: a
 * stock sketch never sends these commands, gets ERROR for them from a stock bridge, and sees no
 * change in behaviour from this one.
 *
 *   AT+TANKVER?        -> +TANKVER: <extension version>,<wifi status as AT+GETSTATUS?>
 *                         Doubles as the status poll and as a heartbeat proving the UART link.
 *   AT+TANKLINK=<0|1>  -> link mode for a robot that streams over one TCP client:
 *                         Wi-Fi modem sleep off (the access point no longer buffers our frames
 *                         for up to a beacon interval), TCP_NODELAY on server sockets, a short
 *                         TCP keepalive on accepted clients (a vanished phone is noticed in about
 *                         10 s instead of 2 h) and a CLIENTSEND that never blocks.
 *   AT+TANKBAUD=<baud> -> OK at the old rate, then the AT UART switches. Falls back to 115200
 *                         after TANK_BAUD_IDLE_REVERT_MS without a command, and on a 1200/2400
 *                         baud touch (sketch upload), so a stock sketch always finds 115200.
 *
 * A non-blocking CLIENTSEND: the stock one waits in WiFiClient::write() for up to 10 x 1 s when
 * the peer stops reading, and the whole AT task (every other command) waits with it. Here the
 * bytes the socket does not take at once are kept (up to TANK_PENDING_CAP per socket) and sent
 * from the AT loop; a send that would not fit is refused whole with ERROR, so the sketch keeps
 * the data and retries without ever duplicating a prefix.
 */

#include "at_handler.h"

#include <errno.h>
#include <lwip/sockets.h>

#define _TANKVER   "+TANKVER"
#define _TANKLINK  "+TANKLINK"
#define _TANKBAUD  "+TANKBAUD"

#define TANK_EXT_VERSION            1
#define TANK_DEFAULT_BAUD           115200
#define TANK_BAUD_IDLE_REVERT_MS    10000
#define TANK_PENDING_CAP            1024
#define TANK_PENDING_SLOTS          4
#define TANK_KEEPALIVE_IDLE_S       4
#define TANK_KEEPALIVE_INTERVAL_S   2
#define TANK_KEEPALIVE_COUNT        3
#define TANK_CLOSE_FLUSH_MS         500

namespace TankLink {

struct Pending {
  int fd = -1;
  std::vector<uint8_t> bytes;
};

static Pending pending[TANK_PENDING_SLOTS];
static volatile bool enabled = false;
static volatile uint32_t requestedBaud = 0;     // applied after the OK has left
static volatile bool defaultsRequested = false; // set from the USB event task (1200/2400 touch)
static uint32_t currentBaud = TANK_DEFAULT_BAUD;
static unsigned long lastCommandMs = 0;

static Pending* slotFor(int fd, bool create) {
  for (auto& p : pending) {
    if (p.fd == fd) {
      return &p;
    }
  }
  if (!create) {
    return nullptr;
  }
  for (auto& p : pending) {
    if (p.fd < 0) {
      p.fd = fd;
      p.bytes.clear();
      return &p;
    }
  }
  return nullptr;
}

static void forget(int fd) {
  Pending* p = slotFor(fd, false);
  if (p != nullptr) {
    p->fd = -1;
    p->bytes.clear();
    p->bytes.shrink_to_fit();
  }
}

static void forgetAll() {
  for (auto& p : pending) {
    p.fd = -1;
    p.bytes.clear();
    p.bytes.shrink_to_fit();
  }
}

// Bytes taken by the socket (0 when its buffer is full), or -1 when the connection is gone.
static int trySend(int fd, const uint8_t* data, size_t length) {
  int sent = lwip_send(fd, data, length, MSG_DONTWAIT);
  if (sent >= 0) {
    return sent;
  }
  if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOMEM) {
    return 0;
  }
  return -1;
}

static void flush(Pending& p) {
  if (p.fd < 0 || p.bytes.empty()) {
    return;
  }
  int sent = trySend(p.fd, p.bytes.data(), p.bytes.size());
  if (sent < 0) {
    forget(p.fd);
    return;
  }
  p.bytes.erase(p.bytes.begin(), p.bytes.begin() + sent);
}

static void flushAll() {
  for (auto& p : pending) {
    flush(p);
  }
}

// All or nothing: true = every byte was sent or is queued here; false = nothing was taken.
static bool send(WiFiClient* client, const uint8_t* data, size_t length) {
  int fd = client->fd();
  if (fd < 0) {
    return false;
  }
  Pending* p = slotFor(fd, true);
  if (p == nullptr) {
    return client->write(data, length) == length;   // more sockets than slots: the stock path
  }
  flush(*p);
  if (p->fd < 0) {
    return false;                                   // the connection died while flushing
  }
  if (!p->bytes.empty()) {
    if (p->bytes.size() + length > TANK_PENDING_CAP) {
      return false;                                 // peer not reading: the sketch keeps the data
    }
    p->bytes.insert(p->bytes.end(), data, data + length);
    return true;
  }
  int sent = trySend(fd, data, length);
  if (sent < 0) {
    forget(fd);
    return false;
  }
  if (static_cast<size_t>(sent) < length) {
    p->bytes.insert(p->bytes.end(), data + sent, data + length);
  }
  return true;
}

// Before a close: give queued bytes a moment to leave (a reply written just before stop()).
static void flushBeforeClose(int fd) {
  Pending* p = slotFor(fd, false);
  unsigned long start = millis();
  while (p != nullptr && p->fd == fd && !p->bytes.empty() && millis() - start < TANK_CLOSE_FLUSH_MS) {
    flush(*p);
    vTaskDelay(1);
  }
  forget(fd);
}

static void tuneAcceptedSocket(int fd) {
  forget(fd);                                       // a reused descriptor must not inherit old bytes
  if (!enabled || fd < 0) {
    return;
  }
  int value = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value));
  value = TANK_KEEPALIVE_IDLE_S;
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &value, sizeof(value));
  value = TANK_KEEPALIVE_INTERVAL_S;
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &value, sizeof(value));
  value = TANK_KEEPALIVE_COUNT;
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &value, sizeof(value));
}

static void setEnabled(bool on) {
  enabled = on;
  WiFi.setSleep(!on);                               // modem sleep is the arduino-esp32 default
  if (!on) {
    forgetAll();
  }
}

// From the USB event task: only flags, the AT task applies them.
static void requestDefaults() {
  defaultsRequested = true;
}

static void noteCommand() {
  lastCommandMs = millis();
}

static bool validBaud(uint32_t baud) {
  return baud == 115200 || baud == 230400 || baud == 460800 || baud == 921600;
}

// Called by the AT task after every pass (after the replies of that pass were queued).
static void afterRun(HardwareSerial* serial) {
  if (defaultsRequested) {
    defaultsRequested = false;
    requestedBaud = 0;
    setEnabled(false);
    if (currentBaud != TANK_DEFAULT_BAUD) {
      serial->flush();
      serial->updateBaudRate(TANK_DEFAULT_BAUD);
      currentBaud = TANK_DEFAULT_BAUD;
    }
  }
  if (requestedBaud != 0) {
    uint32_t baud = requestedBaud;
    requestedBaud = 0;
    serial->flush();                                // the OK leaves at the old rate
    serial->updateBaudRate(baud);
    currentBaud = baud;
    lastCommandMs = millis();
  }
  if (currentBaud != TANK_DEFAULT_BAUD && millis() - lastCommandMs > TANK_BAUD_IDLE_REVERT_MS) {
    serial->updateBaudRate(TANK_DEFAULT_BAUD);      // the sketch is gone (reset, new upload)
    currentBaud = TANK_DEFAULT_BAUD;
  }
  flushAll();
}

}  // namespace TankLink

void CAtHandler::add_cmds_tank() {
  // AT+TANKVER?
  command_table[_TANKVER] = [this](auto & srv, auto & parser) {
    if (parser.cmd_mode != chAT::CommandMode::Read) {
      return chAT::CommandStatus::ERROR;
    }
    srv.write_response_prompt();
    String reply = String(TANK_EXT_VERSION) + "," + String(wifi_status);
    srv.write_str((const char *)reply.c_str());
    srv.write_line_end();
    return chAT::CommandStatus::OK;
  };

  // AT+TANKLINK=<0|1>
  command_table[_TANKLINK] = [this](auto & srv, auto & parser) {
    if (parser.cmd_mode != chAT::CommandMode::Write || parser.args.size() != 1 || parser.args[0].empty()) {
      return chAT::CommandStatus::ERROR;
    }
    bool on = atoi(parser.args[0].c_str()) != 0;
    TankLink::setEnabled(on);
    for (int i = 0; i < MAX_SERVER_AVAILABLE; i++) {
      if (serverWiFi[i] != nullptr) {
        serverWiFi[i]->setNoDelay(on);
      }
    }
    srv.write_response_prompt();
    srv.write_str(on ? "1" : "0");
    srv.write_line_end();
    return chAT::CommandStatus::OK;
  };

  // AT+TANKBAUD=<baud>
  command_table[_TANKBAUD] = [this](auto & srv, auto & parser) {
    if (parser.cmd_mode != chAT::CommandMode::Write || parser.args.size() != 1 || parser.args[0].empty()) {
      return chAT::CommandStatus::ERROR;
    }
    uint32_t baud = strtoul(parser.args[0].c_str(), nullptr, 10);
    if (!TankLink::validBaud(baud)) {
      return chAT::CommandStatus::ERROR;
    }
    TankLink::requestedBaud = baud;
    srv.write_response_prompt();
    srv.write_str((const char *)String(baud).c_str());
    srv.write_line_end();
    return chAT::CommandStatus::OK;
  };

  // A fresh sketch starts with AT+SOFTRESETWIFI: it gets the stock behaviour back.
  auto stockSoftReset = command_table[_SOFTRESETWIFI];
  command_table[_SOFTRESETWIFI] = [this, stockSoftReset](auto & srv, auto & parser) {
    TankLink::setEnabled(false);
    return stockSoftReset(srv, parser);
  };
}

#endif
