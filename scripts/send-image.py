#!/usr/bin/env python3
"""
Send Image Command Client
Sends JSON commands to the image display server via TCP.
"""

import socket
import json
import sys
import argparse


def send_command(host: str, port: int, command: dict) -> dict:
    """Send a command to the server and return the response."""
    try:
        # Create socket connection
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
            sock.connect((host, port))
            
            # Send command as JSON with newline
            command_json = json.dumps(command) + '\n'
            sock.sendall(command_json.encode())
            
            # Receive response
            response_data = sock.recv(4096)
            response = json.loads(response_data.decode().strip())
            
            return response
            
    except ConnectionRefusedError:
        return {
            'status': 'error',
            'message': f'Connection refused. Is the server running on {host}:{port}?'
        }
    except socket.timeout:
        return {
            'status': 'error',
            'message': 'Connection timed out'
        }
    except Exception as e:
        return {
            'status': 'error',
            'message': f'Error: {str(e)}'
        }

def main():
    """Main entry point."""
    parser = argparse.ArgumentParser(
        description='Send commands to the image display server',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Example usage:
    python send-image.py localhost 5555 '{"type":"DISPLAY_IMAGE","path":"C:\\path\\to\\image.png","rotation":180}'
    python send-image.py localhost 5555 '{"type":"DISPLAY_IMAGE","path":"https://picsum.photos/1920/1200","rotation":180}'
    python send-image.py localhost 5555 '{"type":"CLEAR"}'
        """
    )
    
    parser.add_argument('host', help='Server hostname or IP address')
    parser.add_argument('port', type=int, help='Server port number')
    parser.add_argument('command', help='Command to send (STATUS, CLEAR, DISPLAY_IMAGE <path>, or JSON)')
    
    args = parser.parse_args()
    
    try:
        print(f"Connecting to {args.host}:{args.port}")

        command_payload = json.loads(args.command)
        print(f"Sending command: {json.dumps(command_payload, indent=2)}")
        
        # Send command and get response
        response = send_command(args.host, args.port, command_payload)
        
        # Display response
        print(f"\nResponse:")
        print(json.dumps(response, indent=2))
        
        # Exit with error code if command failed
        if response.get('status') == 'error':
            sys.exit(1)
            
    except ValueError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("\nCancelled", file=sys.stderr)
        sys.exit(130)


if __name__ == '__main__':
    main()
