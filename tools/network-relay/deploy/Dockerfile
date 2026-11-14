# Build context is an audited release directory, not the whole checkout.
FROM scratch
COPY cara-relay-websocket /cara-relay
COPY THIRD_PARTY_NOTICES.txt /THIRD_PARTY_NOTICES.txt
USER 65532:65532
ENV GOMAXPROCS=1 GOMEMLIMIT=48MiB
ENTRYPOINT ["/cara-relay"]
