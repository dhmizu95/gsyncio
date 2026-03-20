"""
setup.py - Build script for gsyncio

gsyncio is a high-performance fiber-based concurrency package for Python.

Usage:
    # Build the package
    python setup.py build_ext --inplace
    
    # Install in development mode
    pip install -e .
    
    # Build wheel
    python setup.py bdist_wheel
"""

import os
import sys
from pathlib import Path

from setuptools import setup, Extension, find_packages
from setuptools.command.build_ext import build_ext

# Try to import Cython
try:
    from Cython.Build import cythonize
    HAS_CYTHON = True
except ImportError:
    HAS_CYTHON = False

# Package metadata
NAME = 'gsyncio'
VERSION = '0.1.0'
DESCRIPTION = 'High-performance fiber-based concurrency for Python'
LONG_DESCRIPTION = Path('README.md').read_text() if Path('README.md').exists() else ''
LONG_DESCRIPTION_CONTENT_TYPE = 'text/markdown'
AUTHOR = 'gsyncio team'
AUTHOR_EMAIL = 'gsyncio@example.com'
URL = 'https://github.com/gsyncio/gsyncio'
LICENSE = 'MIT'
PYTHON_REQUIRES = '>=3.8'

# C source files
CSRC_DIR = Path('csrc')
C_SOURCES = [
    str(CSRC_DIR / 'fiber.c'),
    str(CSRC_DIR / 'fiber_pool.c'),
    str(CSRC_DIR / 'scheduler.c'),
    str(CSRC_DIR / 'future.c'),
    str(CSRC_DIR / 'channel.c'),
    str(CSRC_DIR / 'waitgroup.c'),
    str(CSRC_DIR / 'select.c'),
    str(CSRC_DIR / 'task.c'),
    str(CSRC_DIR / 'evloop.c'),
    str(CSRC_DIR / 'net.c'),
    str(CSRC_DIR / 'io_uring.c'),
    str(CSRC_DIR / 'native_io.c'),
    str(CSRC_DIR / 'worker_manager.c'),
    str(CSRC_DIR / 'c_tasks.c'),  # C-based task execution
]

# Compiler flags
CFLAGS = [
    '-O3',
    '-std=c11',
    '-pthread',
    '-fPIC',
    '-Wall',
    '-Wextra',
    '-Wno-unused-parameter',
    '-Wno-unused-function',
]

# Platform-specific flags
if sys.platform == 'darwin':
    CFLAGS.extend([
        '-mmacosx-version-min=10.14',
    ])
elif sys.platform == 'win32':
    CFLAGS = [
        '/O2',
        '/std:c11',
        '/W3',
    ]

# Extension modules
def get_extensions():
    if not HAS_CYTHON:
        return []

    extensions = [
        Extension(
            'gsyncio._gsyncio_core',
            sources=['gsyncio/_gsyncio_core.pyx'] + C_SOURCES,
            include_dirs=[str(CSRC_DIR)],
            extra_compile_args=CFLAGS,
            extra_link_args=['-pthread'],
        ),
        Extension(
            'gsyncio._gsyncio_native_io',
            sources=[
                'gsyncio/_gsyncio_native_io.pyx',
            ] + C_SOURCES,
            include_dirs=[str(CSRC_DIR)],
            extra_compile_args=CFLAGS,
            extra_link_args=['-pthread'],
        ),
    ]

    return cythonize(
        extensions,
        compiler_directives={
            'language_level': 3,
            'boundscheck': False,
            'wraparound': False,
        },
    )


class BuildExt(build_ext):
    """Custom build extension command"""
    
    def build_extensions(self):
        # Check for Cython
        if not HAS_CYTHON:
            print("Warning: Cython not found, building without C extension")
            return
        
        # Add optimization flags
        for ext in self.extensions:
            if hasattr(ext, 'extra_compile_args'):
                if sys.platform != 'win32':
                    ext.extra_compile_args.extend(['-O3', '-march=native'])
        
        super().build_extensions()


# Dependencies
INSTALL_REQUIRES = [
    # No external dependencies for runtime
]

EXTRAS_REQUIRE = {
    'dev': [
        'cython>=0.29.0',
        'pytest>=6.0.0',
        'pytest-asyncio>=0.18.0',
        'pytest-benchmark>=3.4.0',
        'black>=22.0.0',
        'isort>=5.0.0',
        'mypy>=0.950',
    ],
    'docs': [
        'sphinx>=4.0.0',
        'sphinx-rtd-theme>=1.0.0',
    ],
}

# Setup configuration
setup(
    name=NAME,
    version=VERSION,
    description=DESCRIPTION,
    long_description=LONG_DESCRIPTION,
    long_description_content_type=LONG_DESCRIPTION_CONTENT_TYPE,
    author=AUTHOR,
    author_email=AUTHOR_EMAIL,
    url=URL,
    license=LICENSE,
    packages=find_packages(),
    package_data={
        'gsyncio': ['*.pxd', '*.pyx'],
        'gsyncio.csrc': ['*.h', '*.c'],
    },
    ext_modules=get_extensions(),
    cmdclass={
        'build_ext': BuildExt,
    },
    python_requires=PYTHON_REQUIRES,
    install_requires=INSTALL_REQUIRES,
    extras_require=EXTRAS_REQUIRE,
    classifiers=[
        'Development Status :: 3 - Alpha',
        'Intended Audience :: Developers',
        'License :: OSI Approved :: MIT License',
        'Operating System :: POSIX :: Linux',
        'Operating System :: MacOS :: MacOS X',
        'Operating System :: Microsoft :: Windows',
        'Programming Language :: Python :: 3',
        'Programming Language :: Python :: 3.8',
        'Programming Language :: Python :: 3.9',
        'Programming Language :: Python :: 3.10',
        'Programming Language :: Python :: 3.11',
        'Programming Language :: Python :: 3.12',
        'Programming Language :: Python :: Implementation :: CPython',
        'Topic :: Software Development :: Libraries',
        'Topic :: System :: Concurrency',
    ],
    keywords='async concurrency fiber coroutine goroutine asyncio',
)
