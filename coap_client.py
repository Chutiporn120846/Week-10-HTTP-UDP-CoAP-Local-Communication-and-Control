import asyncio
import aiocoap

async def main():
    protocol = await aiocoap.Context.create_client_context()
    try:
        request = aiocoap.Message(
            code=aiocoap.GET,
            uri="coap://192.168.1.100/.well-known/core"
        )
        response = await protocol.request(request).response
        print("Response Code:", response.code)
        print("Content:")
        print(response.payload.decode("utf-8", errors="replace"))
    finally:
        await protocol.shutdown()

asyncio.run(main())
