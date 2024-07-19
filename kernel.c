extern void putchar(char a);

void write(char* string);

extern void kernel()
{
	write("Hello, World!");
}

void write(char* string)
{
	char* p = string;

	for (; *p != 0; p++)
	{
		putchar(*p);
	}
}
