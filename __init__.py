from flask import Flask

app = Flask(__name__)
@app.route('/')
def hello():
    return 'Hear Between The Lines!'

