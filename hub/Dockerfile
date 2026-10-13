FROM python:3.14-slim

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY hub.py .

# non-root
RUN useradd --create-home --uid 10001 hub
USER hub

ENV PANEL_CONFIG=/etc/panel-hub/config.json \
    DEVICE_PORT=8788 \
    HTTP_PORT=8080

EXPOSE 8788 8080

CMD ["python", "hub.py"]
