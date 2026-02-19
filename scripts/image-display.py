#!/usr/bin/env python3
"""
Image Display Server
Displays images and responds to commands via TCP.

Run with "python image-display.py" after filling out "image-display-config.yaml".
Close by pressing ESC.
"""

import asyncio
import json
import logging
import sys
from typing import Optional
import urllib.request
import tempfile
import argparse

import yaml
import pyglet
from PIL import Image
import numpy as np


# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)


def enumerate_monitors():
    display = pyglet.display.get_display()
    screens = display.get_screens()
    
    print("\nAvailable Monitors: index: widthxheight@(x,y)")
    for i, screen in enumerate(screens):
        print(f"Monitor {i}: {screen.width}x{screen.height}@({screen.x},{screen.y})")


def find_monitor(display, monitor_index=None, monitor_size=None, monitor_position=None):
    screens = display.get_screens()
    
    if not screens:
        raise RuntimeError("No monitors found")
    
    # If both size and position specified, match both
    if monitor_size and monitor_position:
        for screen in screens:
            if (screen.width == monitor_size[0] and screen.height == monitor_size[1] and
                screen.x == monitor_position[0] and screen.y == monitor_position[1]):
                logger.info(f"Selected monitor by size {monitor_size} and position {monitor_position}")
                return screen
        logger.warning(f"Monitor with size {monitor_size} and position {monitor_position} not found, using fallback")
    
    # If only size specified, match size
    if monitor_size:
        for screen in screens:
            if screen.width == monitor_size[0] and screen.height == monitor_size[1]:
                logger.info(f"Selected monitor by size {monitor_size}")
                return screen
        logger.warning(f"Monitor with size {monitor_size} not found, using fallback")
    
    # If only position specified, match position
    if monitor_position:
        for screen in screens:
            if screen.x == monitor_position[0] and screen.y == monitor_position[1]:
                logger.info(f"Selected monitor by position {monitor_position}")
                return screen
        logger.warning(f"Monitor at position {monitor_position} not found, using fallback")
    
    # Fallback to index
    if monitor_index is None:
        monitor_index = 0
    
    if monitor_index >= len(screens):
        logger.warning(f"Monitor index {monitor_index} not found, using monitor 0")
        monitor_index = 0
    
    logger.info(f"Selected monitor {monitor_index}")
    return screens[monitor_index]


class ImageDisplayWindow(pyglet.window.Window):
    """Fullscreen window for image display."""
    
    def __init__(self, screen, **kwargs):
        config = pyglet.gl.Config(double_buffer=True, sample_buffers=0, samples=0)
        
        super().__init__(
            width=screen.width,
            height=screen.height,
            caption="Image Display Server",
            fullscreen=True,
            screen=screen,
            vsync=True,
            config=config,
            **kwargs
        )
        
        self.current_sprite = None

    def on_mouse_enter(self, x, y):
        self.set_mouse_visible(False)

    def on_mouse_leave(self, x, y):
        self.set_mouse_visible(True)
        
    def on_draw(self):
        """Render the current image."""
        self.clear()
        
        if self.current_sprite:
            self.current_sprite.draw()
        
    def load_image(self, image_path: str, rotation: int = 0):
        """Load an image from path or URL.
        
        Args:
            image_path: Path or URL to the image
            rotation: Rotation angle in degrees (0, 90, 180, 270)
        """
        try:
            if image_path.startswith(('http://', 'https://')):
                logger.info(f"Downloading image from URL: {image_path}")
                with tempfile.NamedTemporaryFile(delete=False, suffix='.tmp') as tmp_file:
                    urllib.request.urlretrieve(image_path, tmp_file.name)
                    image_path = tmp_file.name
            
            # Load image using PIL
            logger.info(f"Loading image: {image_path}")
            img = Image.open(image_path)
            img = img.convert('RGBA')
            
            if rotation:
                logger.info(f"Rotating image by {rotation} degrees")
                img = img.rotate(-rotation, expand=True)
            
            # Get image data
            img_data = np.array(img)
            height, width = img_data.shape[:2]
            
            # PIL uses top-down, OpenGL uses bottom-up.
            img_data = np.flipud(img_data)
            raw_data = img_data.tobytes()
            
            pyglet_image = pyglet.image.ImageData(
                width, height, 'RGBA', raw_data, pitch=-width * 4
            )
            
            texture = pyglet_image.get_texture()
            
            # Calculate scaling to fit window while maintaining aspect ratio
            image_aspect = width / height
            window_aspect = self.width / self.height
            
            if image_aspect > window_aspect:
                # Image is wider than window
                scale = self.width / width
            else:
                # Image is taller than window
                scale = self.height / height
            
            self.current_sprite = pyglet.sprite.Sprite(
                texture,
                x=0, y=0
            )
            self.current_sprite.scale = scale
            
            # Center the sprite
            scaled_width = width * scale
            scaled_height = height * scale
            self.current_sprite.x = (self.width - scaled_width) / 2
            self.current_sprite.y = (self.height - scaled_height) / 2
            
            logger.info(f"Image loaded successfully: {width}x{height}, scale={scale:.2f}, rotation={rotation}°")
            
        except Exception as e:
            logger.error(f"Failed to load image: {e}")
            raise
    
    def clear_image(self):
        """Clear the current image."""
        self.current_sprite = None


class ImageDisplayServer:
    """TCP server for handling image display commands."""
    
    def __init__(self, config: dict):
        self.config = config
        self.host = config.get('host', '0.0.0.0')
        self.port = config.get('port', 5555)
        
        self.window: Optional[ImageDisplayWindow] = None
        self.server = None
        
    async def handle_client(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        addr = writer.get_extra_info('peername')
        logger.info(f"Client connected: {addr}")
        
        try:
            while True:
                # Read data until newline
                data = await reader.readline()
                if not data:
                    break
                
                # Parse JSON command
                try:
                    command = json.loads(data.decode().strip())
                    logger.info(f"Received command: {command}")
                    
                    response = await self.process_command(command)
                    
                    writer.write(json.dumps(response).encode() + b'\n')
                    await writer.drain()
                    
                except json.JSONDecodeError as e:
                    error_response = {
                        'status': 'error',
                        'message': f'Invalid JSON: {e}'
                    }
                    writer.write(json.dumps(error_response).encode() + b'\n')
                    await writer.drain()
                    
        except asyncio.CancelledError:
            logger.info(f"Client handler cancelled: {addr}")
        except Exception as e:
            logger.error(f"Error handling client {addr}: {e}")
        finally:
            logger.info(f"Client disconnected: {addr}")
            writer.close()
            await writer.wait_closed()
    
    async def process_command(self, command: dict) -> dict:
        """Process a command and return response."""
        cmd_type = command.get('type', '').upper()
        
        if cmd_type == 'DISPLAY_IMAGE':
            return await self.handle_display_image(command)
        elif cmd_type == 'CLEAR':
            return await self.handle_clear()
        else:
            return {
                'status': 'error',
                'message': f'Unknown command type: {cmd_type}'
            }
    
    async def handle_display_image(self, command: dict) -> dict:
        try:
            image_path = command.get('path')
            rotation = command.get('rotation', 0)
            
            if not image_path:
                return {
                    'status': 'error',
                    'message': 'Missing path parameter'
                }

            local_path = self._resolve_image_path(image_path)
            width, height = self._read_image_size(local_path)

            if rotation % 180 != 0:
                rotated_size = (height, width)
            else:
                rotated_size = (width, height)

            expected_size = (self.window.width, self.window.height)
            if rotated_size != expected_size:
                logger.error(
                    "Rejected image: %sx%s (rot=%s) != screen %sx%s",
                    width, height, rotation, expected_size[0], expected_size[1]
                )
                pyglet.clock.schedule_once(lambda dt: self.window.clear_image(), 0)
                return {
                    'status': 'error',
                    'message': (
                        f'Image size {rotated_size[0]}x{rotated_size[1]} does not match '
                        f'screen {expected_size[0]}x{expected_size[1]} at rotation {rotation}°'
                    )
                }

            # Schedule image loading on the main pyglet thread
            pyglet.clock.schedule_once(lambda dt: self.window.load_image(local_path, rotation), 0)
            
            return {
                'status': 'success',
                'message': f'Displaying image: {image_path} (rotation={rotation}°)'
            }
            
        except Exception as e:
            logger.error(f"Error displaying image: {e}")
            return {
                'status': 'error',
                'message': str(e)
            }

    def _resolve_image_path(self, image_path: str) -> str:
        if image_path.startswith(('http://', 'https://')):
            logger.info(f"Downloading image from URL: {image_path}")
            with tempfile.NamedTemporaryFile(delete=False, suffix='.tmp') as tmp_file:
                urllib.request.urlretrieve(image_path, tmp_file.name)
                return tmp_file.name
        return image_path

    def _read_image_size(self, image_path: str) -> tuple[int, int]:
        try:
            with Image.open(image_path) as img:
                return img.size
        except Exception as e:
            raise RuntimeError(f"Failed to read image size: {e}")
    
    async def handle_clear(self) -> dict:
        try:
            if self.window:
                # Schedule clear on the main pyglet thread
                pyglet.clock.schedule_once(lambda dt: self.window.clear_image(), 0)
            
            return {
                'status': 'success',
                'message': 'Image cleared'
            }
        except Exception as e:
            return {
                'status': 'error',
                'message': str(e)
            }
    
    async def start(self):
        """Start the TCP server."""
        self.server = await asyncio.start_server(
            self.handle_client,
            self.host,
            self.port
        )
        
        addr = self.server.sockets[0].getsockname()
        logger.info(f"Server started on {addr[0]}:{addr[1]}")
        
        async with self.server:
            await self.server.serve_forever()

    def create_window(self):
        """Create the display window."""
        display = pyglet.display.get_display()
        screen = find_monitor(
            display,
            monitor_index=self.config.get('monitor_index'),
            monitor_size=self.config.get('monitor_size'),
            monitor_position=self.config.get('monitor_position')
        )
        self.window = ImageDisplayWindow(screen=screen)
        logger.info("Display window created")


def run_async_server(server: ImageDisplayServer, loop: asyncio.AbstractEventLoop):
    """Run the async server in a background thread."""
    asyncio.set_event_loop(loop)
    try:
        loop.run_until_complete(server.start())
    except Exception as e:
        logger.error(f"Server error: {e}")


def main():
    """Main entry point."""
    parser = argparse.ArgumentParser(
        description='Image Display Server - Display images fullscreen via TCP commands'
    )
    parser.add_argument('config', nargs='?', default='image-display-config.yaml',
                        help='Configuration file path (default: image-display-config.yaml)')
    parser.add_argument('--enum-monitors', action='store_true',
                        help='Enumerate available monitors and exit')
    
    args = parser.parse_args()
    
    # Handle monitor enumeration
    if args.enum_monitors:
        enumerate_monitors()
        sys.exit(0)
    
    config_path = args.config
    
    # Load configuration
    try:
        with open(config_path, 'r') as f:
            config = yaml.safe_load(f)
        logger.info(f"Configuration loaded from {config_path}")
    except FileNotFoundError:
        logger.warning(f"Config file not found: {config_path}, using defaults")
        config = {}
    except Exception as e:
        logger.error(f"Error loading config: {e}")
        sys.exit(1)
    
    # Create server
    server = ImageDisplayServer(config)
    
    # Create window on main thread
    server.create_window()
    
    # Start async server in background thread
    import threading
    loop = asyncio.new_event_loop()
    server_thread = threading.Thread(target=run_async_server, args=(server, loop), daemon=True)
    server_thread.start()
    
    # Run pyglet on main thread
    try:
        logger.info("Starting pyglet event loop on main thread")
        pyglet.app.run()
    except KeyboardInterrupt:
        logger.info("Shutting down...")
    finally:
        if server.server:
            loop.call_soon_threadsafe(server.server.close)
        logger.info("Shutdown complete")


if __name__ == '__main__':
    main()
