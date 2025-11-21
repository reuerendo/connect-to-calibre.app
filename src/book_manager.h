#ifndef BOOK_MANAGER_H
#define BOOK_MANAGER_H

#include <string>
#include <vector>
#include <map>

// Book metadata structure
struct BookMetadata {
    std::string uuid;
    std::string title;
    std::string authors;
    std::string lpath;           // Logical path on device
    std::string series;
    int seriesIndex;
    std::string publisher;
    std::string pubdate;
    std::string lastModified;
    std::string tags;
    std::string comments;
    long long size;
    std::string thumbnail;       // Base64 encoded cover
    int thumbnailHeight;
    int thumbnailWidth;
    
    // Sync fields
    bool isRead;
    std::string lastReadDate;
    bool isFavorite;
    
    BookMetadata() : seriesIndex(0), size(0), thumbnailHeight(0), 
                     thumbnailWidth(0), isRead(false), isFavorite(false) {}
};

// Book manager for database operations
class BookManager {
public:
    BookManager();
    ~BookManager();
    
    // Initialize database
    bool initialize(const std::string& dbPath);
    
    // Book operations
    bool addBook(const BookMetadata& metadata);
    bool updateBook(const BookMetadata& metadata);
    bool deleteBook(const std::string& uuid);
    bool getBook(const std::string& uuid, BookMetadata& metadata);
    bool bookExists(const std::string& uuid);
    
    // Get all books
    std::vector<BookMetadata> getAllBooks();
    int getBookCount();
    
    // Metadata cache operations
    bool hasMetadataChanged(const BookMetadata& metadata);
    void updateMetadataCache(const BookMetadata& metadata);
    
    // File operations
    std::string getBookFilePath(const std::string& lpath);
    bool saveBookFile(const std::string& lpath, const void* data, size_t length);
    bool deleteBookFile(const std::string& lpath);
    
    // Collection operations
    void updateCollections(const std::map<std::string, std::vector<std::string>>& collections);
    
private:
    void* db;  // SQLite database handle
    std::string booksDir;
    
    bool createTables();
    std::string sanitizeFileName(const std::string& name);
};

#endif // BOOK_MANAGER_H