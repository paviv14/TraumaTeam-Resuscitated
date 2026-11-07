"""ttre - Trauma Team (Wii) reverse-engineering toolkit."""
import os
from .msg2 import load_tag_names

DATA = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "data")


def setup():
    load_tag_names(os.path.join(DATA, "msgtags.csv"))
