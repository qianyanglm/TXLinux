
int main()
{
    const char *v = event_get_version();
    std::cout << "当前libevent版本:" << v << std::endl;
}