/*
  WiFiClient.cpp - Library for Arduino Wifi shield.
  Copyright (c) 2011-2014 Arduino.  All right reserved.

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

extern "C" {
	#include "socket/include/socket.h"
	#include "driver/include/m2m_periph.h"
}

#include "Adafruit_WINC1500.h"
#include "Adafruit_WINC1500Client.h"

#define IS_CONNECTED	(_flag & SOCKET_BUFFER_FLAG_CONNECTED)

Adafruit_WINC1500Client::Adafruit_WINC1500Client()
{
	_socket = -1;
	_flag = 0;
	_head = 0;
	_tail = 0;
}

Adafruit_WINC1500Client::Adafruit_WINC1500Client(uint8_t sock, uint8_t parentsock)
{
	// Spawn connected TCP client from TCP server socket:
	_socket = sock;
	_flag = SOCKET_BUFFER_FLAG_CONNECTED;
	if (parentsock) {
		_flag |= ((uint32_t)(parentsock - 1)) << SOCKET_BUFFER_FLAG_PARENT_SOCKET_POS;
	}
	_head = 0;
	_tail = 0;
	for (int sock = 0; sock < TCP_SOCK_MAX; sock++) {
		if (WiFi._client[sock] == this)
			WiFi._client[sock] = 0;
	}
	WiFi._client[_socket] = this;
	
	// Add socket buffer handler:
	socketBufferRegister(_socket, &_flag, &_head, &_tail, (uint8 *)_buffer);
	
	// Enable receive buffer:
	recv(_socket, _buffer, SOCKET_BUFFER_MTU, 0);

	m2m_wifi_handle_events(NULL);
}

Adafruit_WINC1500Client::Adafruit_WINC1500Client(const Adafruit_WINC1500Client& other)
{
	copyFrom(other);
}

void Adafruit_WINC1500Client::copyFrom(const Adafruit_WINC1500Client& other)
{
	_socket = other._socket;
	_flag = other._flag;
	_head = other._head;
	_tail = other._tail;
	if (_head > _tail) {
		memcpy(_buffer + _tail, other._buffer + _tail, (_head - _tail));
	}

	for (int sock = 0; sock < TCP_SOCK_MAX; sock++) {
		if (WiFi._client[sock] == this)
			WiFi._client[sock] = 0;
	}

	if (_socket > -1) {
		WiFi._client[_socket] = this;
		
		// Add socket buffer handler:
		socketBufferRegister(_socket, &_flag, &_head, &_tail, (uint8 *)_buffer);
		
		// Enable receive buffer:
		recv(_socket, _buffer, SOCKET_BUFFER_MTU, 0);
	}

	m2m_wifi_handle_events(NULL);
}

int Adafruit_WINC1500Client::connectSSL(const char* host, uint16_t port)
{
	return connect(host, port, SOCKET_FLAGS_SSL);
}

int Adafruit_WINC1500Client::connectSSL(IPAddress ip, uint16_t port)
{
	return connect(ip, port, SOCKET_FLAGS_SSL, 0);
}

int Adafruit_WINC1500Client::connect(const char* host, uint16_t port)
{
	return connect(host, port, 0);
}

int Adafruit_WINC1500Client::connect(IPAddress ip, uint16_t port)
{
	return connect(ip, port, 0, 0);	
}

int Adafruit_WINC1500Client::connect(const char* host, uint16_t port, uint8_t opt)
{
	IPAddress remote_addr;
	if (WiFi.hostByName(host, remote_addr)) {
		return connect(remote_addr, port, opt, (const uint8_t *)host);
	}
	return 0;
}

int Adafruit_WINC1500Client::connect(IPAddress ip, uint16_t port, uint8_t opt, const uint8_t *hostname)
{
	struct sockaddr_in addr;

	// Initialize socket address structure:
	addr.sin_family = AF_INET;
	addr.sin_port = _htons(port);
	addr.sin_addr.s_addr = ip;

	// Create TCP socket:
	_flag = 0;
	_head = 0;
	_tail = 0;
	if ((_socket = socket(AF_INET, SOCK_STREAM, opt)) < 0) {
		return 0;
	}

	if (opt & SOCKET_FLAGS_SSL && hostname) {
		setsockopt(_socket, SOL_SSL_SOCKET, SO_SSL_SNI, hostname, m2m_strlen((uint8_t *)hostname));
	}

	// Add socket buffer handler:
	socketBufferRegister(_socket, &_flag, &_head, &_tail, (uint8 *)_buffer);

	// Connect to remote host:
	if (connectSocket(_socket, (struct sockaddr *)&addr, sizeof(struct sockaddr_in)) < 0) {
		close(_socket);
		_socket = -1;
		return 0;
	}
	
	// Wait for connection or timeout:
	unsigned long start = millis();
	while (!IS_CONNECTED && millis() - start < 20000) {
		m2m_wifi_handle_events(NULL);
	}
	if (!IS_CONNECTED) {
		close(_socket);
		_socket = -1;
		return 0;
	}

	return 1;
}

size_t Adafruit_WINC1500Client::write(uint8_t b)
{
	return write(&b, 1);
}

size_t Adafruit_WINC1500Client::writeAsync(uint8_t b)
{
	return write(&b, 1);
}

size_t Adafruit_WINC1500Client::write(const uint8_t *buf, size_t size)
{
	sint16 err;

	if (_socket < 0 || size == 0) {
		setWriteError();
		return 0;
	}

	// Network led ON (rev A then rev B).
	m2m_periph_gpio_set_val(M2M_PERIPH_GPIO16, 0);
	m2m_periph_gpio_set_val(M2M_PERIPH_GPIO5, 0);

	m2m_wifi_handle_events(NULL);

	while ((err = send(_socket, (void *)buf, size, 0)) < 0) {
		// Exit on fatal error, retry if buffer not ready.
		if (err != SOCK_ERR_BUFFER_FULL) {
			setWriteError();
			m2m_periph_gpio_set_val(M2M_PERIPH_GPIO16, 1);
			m2m_periph_gpio_set_val(M2M_PERIPH_GPIO5, 1);
			return 0;
		}
		m2m_wifi_handle_events(NULL);
	}
	
	// Network led OFF (rev A then rev B).
	m2m_periph_gpio_set_val(M2M_PERIPH_GPIO16, 1);
	m2m_periph_gpio_set_val(M2M_PERIPH_GPIO5, 1);
			
	return size;
}

size_t Adafruit_WINC1500Client::writeAsync(const uint8_t *buf, size_t size)
{
	sint16 err;

	if (_socket < 0 || size == 0) {
		setWriteError();
		return 0;
	}

	// Network led ON (rev A then rev B).
	m2m_periph_gpio_set_val(M2M_PERIPH_GPIO16, 0);
	m2m_periph_gpio_set_val(M2M_PERIPH_GPIO5, 0);

	m2m_wifi_handle_events(NULL);

	if ((err = send(_socket, (void *)buf, size, 0)) < 0) {
		// Exit on fatal error
		if (err != SOCK_ERR_BUFFER_FULL) {
			setWriteError();
			m2m_periph_gpio_set_val(M2M_PERIPH_GPIO16, 1);
			m2m_periph_gpio_set_val(M2M_PERIPH_GPIO5, 1);
		}
        return err;
	}

	m2m_wifi_handle_events(NULL);
	
	// Network led OFF (rev A then rev B).
	m2m_periph_gpio_set_val(M2M_PERIPH_GPIO16, 1);
	m2m_periph_gpio_set_val(M2M_PERIPH_GPIO5, 1);
			
	return size;
}

int Adafruit_WINC1500Client::available()
{
	m2m_wifi_handle_events(NULL);
	
	if (_socket != -1) {
		return _head - _tail;
	}
	return 0;
}

int Adafruit_WINC1500Client::read()
{
	uint8_t b;

	if (read(&b, sizeof(b)) == -1) {
		return -1;
	}

	return b;
}

int Adafruit_WINC1500Client::read(uint8_t* buf, size_t size)
{
	// sizeof(size_t) is architecture dependent
	// but we need a 16 bit data type here
	uint16_t size_tmp = available();
	
	if (size_tmp == 0) {
		return -1;
	}
	
	if (size < size_tmp) {
		size_tmp = size;
	}

	for (uint32_t i = 0; i < size_tmp; ++i) {
		buf[i] = _buffer[_tail++];
	}
	
	if (_tail == _head) {
		_tail = _head = 0;
		_flag &= ~SOCKET_BUFFER_FLAG_FULL;
		recv(_socket, _buffer, SOCKET_BUFFER_MTU, 0);
		m2m_wifi_handle_events(NULL);
	}

	return size_tmp;
}

int Adafruit_WINC1500Client::peek()
{
	if (!available())
		return -1;

	return _buffer[_tail];
}

void Adafruit_WINC1500Client::flush()
{
	while (available())
		read();
}

void Adafruit_WINC1500Client::stop()
{
	if (_socket < 0)
		return;
		
	socketBufferUnregister(_socket);
	close(_socket);
	_socket = -1;
	_flag = 0;
}

uint8_t Adafruit_WINC1500Client::connected()
{
	m2m_wifi_handle_events(NULL);
	if (available())
		return 1;
	return IS_CONNECTED;
}

uint8_t Adafruit_WINC1500Client::status()
{
	// Deprecated.
	return 0;
}

Adafruit_WINC1500Client::operator bool()
{
	return _socket != -1;
}

Adafruit_WINC1500Client& Adafruit_WINC1500Client::operator =(const Adafruit_WINC1500Client& other)
{
	copyFrom(other);

	return *this;
}
