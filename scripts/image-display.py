#!/usr/bin/env python3
"""
Image Display Server
Displays images and responds to commands via TCP.

Run with "python image-display.py" after filling out "image-display-config.yaml".
Close by pressing ESC.
"""

from __future__ import annotations

import asyncio
import json
import logging
import os
import re
import subprocess
import sys
from typing import Optional
import urllib.request
import tempfile
import argparse

import yaml
import pyglet
from PIL import Image
import numpy as np

from sla_video_runtime import VideoRegistry


# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)

PROJECTOR_HINTS = (
    'projector',
    'epson',
    'benq',
    'optoma',
    'viewsonic',
    'vivitek',
    'infocus',
    'nec',
)


def default_video_cache_dir() -> str:
    """Pick a cache directory that matches common MainsailOS layouts."""
    candidate_roots = [
        os.environ.get('BIOSLICER_CACHE_ROOT'),
        '/home/pi/printer_data/cache',
        os.path.join(os.path.expanduser('~'), 'printer_data', 'cache'),
    ]

    for root in candidate_roots:
        if root and os.path.isdir(root):
            return os.path.join(root, 'bioslicer-sla-video-cache')

    return '/tmp/bioslicer-sla-video-cache'


def _screen_signature(screen) -> tuple[int, int, int, int]:
    return (int(screen.width), int(screen.height), int(screen.x), int(screen.y))


def _parse_geometry(text: Optional[str]) -> Optional[tuple[int, int, int, int]]:
    if not text:
        return None
    match = re.match(r'^(\d+)x(\d+)\+(-?\d+)\+(-?\d+)$', text)
    if not match:
        return None
    return (
        int(match.group(1)),
        int(match.group(2)),
        int(match.group(3)),
        int(match.group(4)),
    )


def _xrandr_connected_outputs() -> list[dict]:
    try:
        result = subprocess.run(
            ['xrandr', '--query', '--verbose'],
            check=False,
            capture_output=True,
            text=True,
        )
    except OSError:
        return []

    if result.returncode != 0:
        return []

    outputs = []
    current = None
    line_re = re.compile(
        r'^([A-Za-z0-9_.-]+)\s+connected(?:\s+primary)?(?:\s+(\d+x\d+\+-?\d+\+-?\d+))?'
    )
    for raw_line in result.stdout.splitlines():
        line = raw_line.rstrip('\n')
        match = line_re.match(line)
        if match:
            current = {
                'name': match.group(1),
                'primary': ' primary ' in f' {line} ',
                'geometry': _parse_geometry(match.group(2)),
                'descriptor': match.group(1).lower(),
            }
            outputs.append(current)
            continue

        if current and line.startswith((' ', '\t')):
            lower = line.strip().lower()
            if any(key in lower for key in ('monitor name', 'identifier', 'model', 'vendor', 'manufacturer')):
                current['descriptor'] += f' {lower}'

    return outputs


def _score_projector_output(output: dict) -> int:
    name = output.get('name', '').lower()
    descriptor = output.get('descriptor', '')
    geometry = output.get('geometry')

    score = 0
    if any(hint in descriptor for hint in PROJECTOR_HINTS):
        score += 300
    if name.startswith('hdmi'):
        score += 70
    elif name.startswith('dp'):
        score += 35
    if not output.get('primary', False):
        score += 45
    if geometry and (geometry[2] != 0 or geometry[3] != 0):
        score += 30
    if geometry and geometry[0] * geometry[1] >= 1920 * 1080:
        score += 15

    return score


def guess_projector_monitor(screens):
    outputs = _xrandr_connected_outputs()
    best = None
    best_score = -1

    for output in outputs:
        geometry = output.get('geometry')
        if not geometry:
            continue
        matched_screen = next((s for s in screens if _screen_signature(s) == geometry), None)
        if not matched_screen:
            continue

        score = _score_projector_output(output)
        if score > best_score:
            best = (matched_screen, output)
            best_score = score

    if best is not None:
        screen, output = best
        logger.info(
            'Auto-selected monitor using xrandr heuristic: %s %sx%s@(%s,%s)',
            output.get('name', 'unknown'),
            screen.width,
            screen.height,
            screen.x,
            screen.y,
        )
        return screen

    secondary_screens = [s for s in screens if (int(s.x), int(s.y)) != (0, 0)]
    if secondary_screens:
        chosen = max(secondary_screens, key=lambda s: int(s.width) * int(s.height))
        logger.info(
            'Auto-selected non-primary monitor fallback: %sx%s@(%s,%s)',
            chosen.width,
            chosen.height,
            chosen.x,
            chosen.y,
        )
        return chosen

    chosen = max(screens, key=lambda s: int(s.width) * int(s.height))
    logger.info(
        'Auto-selected largest monitor fallback: %sx%s@(%s,%s)',
        chosen.width,
        chosen.height,
        chosen.x,
        chosen.y,
    )
    return chosen


def enumerate_monitors():
    display = pyglet.display.get_display()
    screens = display.get_screens()
    
    print("\nAvailable Monitors: index: widthxheight@(x,y)")
    for i, screen in enumerate(screens):
        print(f"Monitor {i}: {screen.width}x{screen.height}@({screen.x},{screen.y})")

    if screens:
        guessed = guess_projector_monitor(screens)
        idx = next((i for i, screen in enumerate(screens) if screen == guessed), 0)
        print(f"Auto-detect guess: Monitor {idx}")


def find_monitor(display, monitor_index=None, monitor_size=None, monitor_position=None, monitor_auto_detect=True):
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

    if monitor_index is None and monitor_auto_detect:
        return guess_projector_monitor(screens)

    if monitor_index is not None:
        try:
            monitor_index = int(monitor_index)
        except (TypeError, ValueError):
            logger.warning("Invalid monitor index %r, using monitor 0", monitor_index)
            monitor_index = 0
    
    # Fallback to index
    if monitor_index is None:
        monitor_index = 0

    if monitor_index < 0:
        logger.warning(f"Negative monitor index {monitor_index} is invalid, using monitor 0")
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

    def _set_texture(self, texture, width: int, height: int):
        image_aspect = width / height
        window_aspect = self.width / self.height

        if image_aspect > window_aspect:
            scale = self.width / width
        else:
            scale = self.height / height

        self.current_sprite = pyglet.sprite.Sprite(texture, x=0, y=0)
        self.current_sprite.scale = scale

        scaled_width = width * scale
        scaled_height = height * scale
        self.current_sprite.x = (self.width - scaled_width) / 2
        self.current_sprite.y = (self.height - scaled_height) / 2
        
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

            self._set_texture(texture, width, height)
            
            logger.info(f"Image loaded successfully: {width}x{height}, rotation={rotation}°")
            
        except Exception as e:
            logger.error(f"Failed to load image: {e}")
            raise

    def load_rgba_frame(self, width: int, height: int, frame_rgba: bytes):
        """Load a raw RGBA frame and render it fullscreen."""
        try:
            frame = np.frombuffer(frame_rgba, dtype=np.uint8)
            frame = frame.reshape((height, width, 4))
            frame = np.flipud(frame)

            pyglet_image = pyglet.image.ImageData(
                width,
                height,
                'RGBA',
                frame.tobytes(),
                pitch=-width * 4
            )

            texture = pyglet_image.get_texture()
            self._set_texture(texture, width, height)
        except Exception as e:
            logger.error(f"Failed to load RGBA frame: {e}")
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
        self.require_exact_resolution = bool(config.get('require_exact_resolution', True))
        self.monitor_auto_detect = bool(config.get('monitor_auto_detect', True))

        cache_dir = config.get('video_cache_dir')
        if not cache_dir:
            cache_dir = default_video_cache_dir()
            logger.info("Using default video cache dir: %s", cache_dir)
        
        self.window: Optional[ImageDisplayWindow] = None
        self.server = None

        self.video_registry = VideoRegistry(
            cache_dir=cache_dir,
            hwaccel=config.get('ffmpeg_hwaccel', 'auto'),
            hw_decoder=config.get('ffmpeg_hw_decoder'),
        )
        
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
        elif cmd_type == 'LOAD_VIDEO':
            return await self.handle_load_video(command)
        elif cmd_type == 'LOAD_VIDEOS_FROM_GCODE':
            return await self.handle_load_videos_from_gcode(command)
        elif cmd_type == 'SHOW_VIDEO_FRAME':
            return await self.handle_show_video_frame(command)
        elif cmd_type == 'UNLOAD_VIDEO':
            return await self.handle_unload_video(command)
        elif cmd_type == 'LIST_VIDEOS':
            return await self.handle_list_videos()
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
            if self.require_exact_resolution and rotated_size != expected_size:
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

    async def handle_load_video(self, command: dict) -> dict:
        try:
            name = command.get('name')
            path = command.get('path')
            if not name:
                return {'status': 'error', 'message': 'Missing name parameter'}
            if not path:
                return {'status': 'error', 'message': 'Missing path parameter'}

            meta = await asyncio.to_thread(
                self.video_registry.load_video,
                str(name),
                str(path),
                command.get('hwaccel'),
                command.get('hw_decoder'),
            )
            return {'status': 'success', 'video': meta}
        except Exception as e:
            logger.error(f"Error loading video: {e}")
            return {'status': 'error', 'message': str(e)}

    async def handle_load_videos_from_gcode(self, command: dict) -> dict:
        try:
            gcode_path = command.get('gcode_path')
            if not gcode_path:
                return {'status': 'error', 'message': 'Missing gcode_path parameter'}

            loaded = await asyncio.to_thread(
                self.video_registry.load_videos_from_gcode,
                str(gcode_path),
            )
            return {'status': 'success', 'loaded': loaded}
        except Exception as e:
            logger.error(f"Error loading videos from G-code: {e}")
            return {'status': 'error', 'message': str(e)}

    async def handle_show_video_frame(self, command: dict) -> dict:
        try:
            name = command.get('name')
            frame = command.get('frame')
            if not name:
                return {'status': 'error', 'message': 'Missing name parameter'}
            if frame is None:
                return {'status': 'error', 'message': 'Missing frame parameter'}

            frame_index = int(frame)
            width, height, rgba = await asyncio.to_thread(
                self.video_registry.get_frame,
                str(name),
                frame_index,
            )

            expected_size = (self.window.width, self.window.height)
            if self.require_exact_resolution and (width, height) != expected_size:
                return {
                    'status': 'error',
                    'message': (
                        f'Frame size {width}x{height} does not match screen '
                        f'{expected_size[0]}x{expected_size[1]}'
                    )
                }

            pyglet.clock.schedule_once(
                lambda dt: self.window.load_rgba_frame(width, height, rgba),
                0,
            )
            return {
                'status': 'success',
                'message': f'Displayed frame {frame_index} from {name}',
                'frame': frame_index,
                'name': name,
            }
        except Exception as e:
            logger.error(f"Error showing video frame: {e}")
            return {'status': 'error', 'message': str(e)}

    async def handle_unload_video(self, command: dict) -> dict:
        try:
            name = command.get('name')
            if not name:
                return {'status': 'error', 'message': 'Missing name parameter'}

            unloaded = await asyncio.to_thread(self.video_registry.unload_video, str(name))
            if unloaded:
                return {'status': 'success', 'message': f'Unloaded {name}'}
            return {'status': 'error', 'message': f'Video not loaded: {name}'}
        except Exception as e:
            return {'status': 'error', 'message': str(e)}

    async def handle_list_videos(self) -> dict:
        try:
            videos = await asyncio.to_thread(self.video_registry.list_videos)
            return {'status': 'success', 'videos': videos}
        except Exception as e:
            return {'status': 'error', 'message': str(e)}

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
            monitor_position=self.config.get('monitor_position'),
            monitor_auto_detect=self.monitor_auto_detect,
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
        server.video_registry.close()
        logger.info("Shutdown complete")


if __name__ == '__main__':
    main()
