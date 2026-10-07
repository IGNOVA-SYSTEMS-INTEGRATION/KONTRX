"""
Pytest configuration and CLI fixtures for Kontrx test suite.
"""
import pytest

DEFAULT_DEVICE = "192.168.1.200"
DEFAULT_USER = "admin"
DEFAULT_PASS = "adminkontrx"

def pytest_addoption(parser):
    parser.addoption("--device", default=DEFAULT_DEVICE, help="Kontrx device IP address")
    parser.addoption("--user", default=DEFAULT_USER, help="Admin username")
    parser.addoption("--password", default=DEFAULT_PASS, help="Admin password")
