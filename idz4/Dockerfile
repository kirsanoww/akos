FROM python:3.13-slim-bookworm
RUN apt-get update && apt-get install -y --no-install-recommends gcc make libc6-dev && rm -rf /var/lib/apt/lists/*
WORKDIR /project
COPY Makefile ./
COPY src ./src
COPY tests ./tests
COPY scripts ./scripts
RUN make
CMD ["make", "test"]
