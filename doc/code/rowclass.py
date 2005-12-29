from pysqlite2 import dbapi2 as sqlite

con = sqlite.connect("mydb")
con.row_factory = sqlite.Row

cur = con.cursor()
cur.execute("select name_last, age from people")
for row in cur:
    assert row[0] == row["name_last"]
    assert row["name_last"] == row["nAmE_lAsT"]
    assert row[1] == row["age"]
    assert row[1] == row["AgE"]
