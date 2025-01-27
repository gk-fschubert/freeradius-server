#!/usr/bin/env python3
import multiprocessing
import time
import socket
import ssl
import logging
from datetime import datetime

class SSLClient:
    def __init__(self, ip, client_cert_file_location, trusted_cas_file_location):
        self.client_cert_file_location = client_cert_file_location
        self.trusted_cas_file_location = trusted_cas_file_location
        self.socket: ssl.SSLSocket = self.connect_to_server(ip, 2083, self.client_cert_file_location, self.trusted_cas_file_location)


    def connect_to_server(self, ip, port, client_cert_file, ca_file_location): 
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            context.verify_mode = ssl.CERT_REQUIRED
            context.load_cert_chain(certfile=client_cert_file, password='whatever')

            context.load_verify_locations(cafile=ca_file_location)
            context.check_hostname = False
            sock = context.wrap_socket(sock, server_hostname=ip)
            
            connection =  sock.connect((ip, int(port)))  
            return sock
            
        except Exception as e:
            logging.error(f'Radius tcp monitor: RADIUS server {ip} is DOWN. err: ' + str(e))
            return None


def send_stuff(x):
    while True:
        try:
            now = datetime.now()
            test = SSLClient('127.0.0.1', '../../../raddb/certs/client.pem', '../../../raddb/certs/ca.pem')
            test.socket.send(b'asdlkfjasldkfj')
            test.socket.shutdown(socket.SHUT_WR)
            # don't wait until a clean shutdown is done and close it directly
            test.socket.close()

        except Exception as e:
            print(f'dur: {(datetime.now() - now)} _ error while sending things: {str(e)}')
        finally:
            try:
                if not hasattr(test.socket, '_closed'):
                    print(f'dur: {(datetime.now() - now)} _ socket not correctly initialized')
                elif not test.socket._closed:
                        test.socket.close()
                print(f"dur: {(datetime.now() - now)}")
            except Exception as e:
                print(f'dur: {(datetime.now() - now)} _ error while closing socket: {str(e)}')
                pass
        time.sleep(0.2)



if __name__ == '__main__':
    with multiprocessing.Pool(16) as p:
        while True:
            try:
                p.map(send_stuff, range(16))
            except KeyboardInterrupt:
                logging.info("Keyboard interrupt")
                break
            except Exception as e:
                logging.error(f"Not able to spwan process: {e}")

