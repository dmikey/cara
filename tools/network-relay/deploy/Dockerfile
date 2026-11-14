# Build context is an audited release directory, not the whole checkout.
FROM scratch
COPY cara-relay-websocket /cara-relay
COPY THIRD_PARTY_NOTICES.txt /THIRD_PARTY_NOTICES.txt
USER 65532:65532
ENV GOMAXPROCS=1 GOMEMLIMIT=48MiB
HEALTHCHECK --interval=15s --timeout=5s --start-period=5s --retries=3 CMD ["/cara-relay", "-healthcheck", "http://127.0.0.1:24874/healthz", "-public-host", "carap2p.therad.ninja"]
ENTRYPOINT ["/cara-relay"]
