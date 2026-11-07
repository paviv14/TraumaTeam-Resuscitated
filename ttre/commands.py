"""Script command (COMM) database: data/commands.csv.

Columns: id, name, argc, msg_args (argument positions holding a message index), handler,
         confidence, evidence
  confidence: source  = name taken from Atlus's leaked test.scr
              inferred = named by us from handler code / usage; may be refined
              (blank)  = unnamed, prints as CMD_XXX
"""
import csv
import os

DATA = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "data")


class Commands:
    def __init__(self, path=os.path.join(DATA, "commands.csv")):
        self.rows = {}
        with open(path, encoding="utf-8") as fh:
            for r in csv.DictReader(fh):
                self.rows[int(r["id"], 16)] = r
        self.by_name = {r["name"]: i for i, r in self.rows.items() if r["name"]}

    def name(self, i):
        r = self.rows.get(i)
        return r["name"] if r and r["name"] else f"CMD_{i:03X}"

    def argc(self, i):
        r = self.rows.get(i)
        return int(r["argc"]) if r else 0

    def msg_args(self, i):
        r = self.rows.get(i)
        return [int(x) for x in r["msg_args"].split(";")] if r and r.get("msg_args") else []

    def id(self, s):
        if s in self.by_name:
            return self.by_name[s]
        if s.startswith("CMD_"):
            return int(s[4:], 16)
        return int(s, 0)
