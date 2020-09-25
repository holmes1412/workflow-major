# Asynchronous MySQL client: mysql_cli

# Sample code

[tutorial-12-mysql_cli.cc](../tutorial/tutorial-12-mysql_cli.cc)

# About mysql_cli

The usage of mysql_cli in the tutorial is similar to that of the official client, which is a command line interactive asynchronous MySQL client.

Program operation mode: ./mysql_cli<URL>

After startup, you can directly enter the mysql command in the terminal to interact with db, and enter quit or Ctrl-C to exit.

# Format of MySQL URL

mysql://username:password@host:port/dbname?character_set=charset
  * Fill in username and password as required;
  * The port is 3306 by default;
  * Dbname is the name of the database to be used. Generally, we recommend filling it in if the SQL statement only operates on one db;
  * If users have upstream selection requirements at this level, they can refer to [upstream document](../docs/about-upstream.md);
  * charset is the character set, utf8 by default. For details, please refer to the official MySQL document [character-set.html](https://dev.mysql.com/doc/internals/en/character-set.html).

MySQL URL example:

mysql://root:password@127.0.0.1

mysql://@test.mysql.com:3306/db1?character_set=utf8

# Create and start MySQL task

User can use WFTaskFactory to create MySQL tasks. The usage of creating interfaces and callback functions are similar to that of other tasks in workflow:

~~~cpp
using mysql_callback_t = std::function<void (WFMySQLTask *)>;

WFMySQLTask *create_mysql_task(const std::string& url, int retry_max, mysql_callback_t callback);

void set_query(const std::string& query);
~~~

After creating the WFMySQLTask, users can call set_query() on req to write SQL statements.

Based on the MySQL protocol, if an empty packet is sent after the connection is established, the server will wait instead of returning the packet, so users will get a timeout. Therefore, we made a special judgment on those tasks that did not call set_query() and immediately returned WFT_ERR_MYSQL_QUERY_NOT_SET.

The usages of callback, series, user_data, etc. are similar to that of other tasks in workflow.

The usages are as below:

~~~cpp
int main(int argc, char *argv[])
{
    ...
    WFMySQLTask *task = WFTaskFactory::create_mysql_task(url, RETRY_MAX, mysql_callback);
    task->get_req()->set_query("SHOW TABLES;");
    ...
    task->start();
    ...
}
~~~

# Supported commands

Currently the supported command is COM_QUERY, which can already cover the user's basic needs for adding, deleting, modifying, query, building and deleting databases, creating tables, preparing, using and storing procedures, and using transactions.

Since the interactive commands do not support database selection (USE command), if there is a cross-database operation involved in the SQL statement, you can specify which table in which library by means of db_name.table_name.

Multiple commands can be concatenated together and passed to WFMySQLTask through set_query(). Generally speaking, multiple statements can get all the results back at one time. However, as the packet return method in the MySQL protocol is not compatible with question and answer communication under some provisions, so the SQL statement in set_query() has the following precautions:

  * Allows for concatenation of multiple single result set statements (general INSERT/UPDATE/SELECT/PREPARE)
  * It can also be a multiple result set statement (such as CALL storage procedure)
  * In other cases, we recommend separating SQL statement for multiple requests
  
For example:

~~~cpp
// Concatenation of multiple single result set statements, which is possible to get correct return result
req->set_query("SELECT * FROM table1; SELECT * FROM table2; INSERT INTO table3 (id) VALUES (1);");

// Single statement with multiple result sets, which is possible to get correct return result
req->set_query("CALL procedure1();");

// Multiple result set and other concatenation, which is impossible to get all return result completely
req->set_query("CALL procedure1(); SELECT * FROM table1;");
~~~

# Result resolve

Similar to other tasks of workflow, you can use task->get_resp() to get MySQLResponse. We can traverse the result set and MySQLField, the information of each column, each row, and each MySQLCell through MySQLResultCursor. For detail of specific interface refer to: [MySQLResult.h](../src/protocol/MySQLResult.h).

Usage steps from outside to inside should be:

1. Judge task state (representing the state of communication level): User checks whether the task execution is successful by judging whether task->get_state() is equal to WFT_STATE_SUCCESS;
2. Determine the type of reply packet (representing the state of returned packet analysis): call resp->get_packet_type() to view the type of MySQL return packet. Common types include:
  * MYSQL_PACKET_OK: Request for returning non-result set: resolve successful;
  * MYSQL_PACKET_EOF: Request for returning result set: resolve successful;
  * MYSQL_PACKET_ERROR: Request: failed;
3. Judging the state of the result set (representing the reading state of the result set): Users can use MySQLResultCursor to read the content in the result set, as the data returned by the MySQL server is multiple result sets, the cursor will automatically point to the read position of the first result set at the beginning. The states can be obtained through cursor->get_cursor_status():
  * MYSQL_STATUS_GET_RESULT: There is data to read;
  * MYSQL_STATUS_END: The last row of the current result set has been read;
  * MYSQL_STATUS_EOF: All result sets have been fetched;
  * MYSQL_STATUS_OK: This reply packet is a non-result set package, no need to read data through the result set interface;
  * MYSQL_STATUS_ERROR: Resolve error;
4. Read each field in columns:
  * int get_field_count() const;
  * const MySQLField *fetch_field();
    * const MySQLField *const *fetch_fields() const;
5. Read each line: Use cursor->fetch_row() to read by row until the return value is false. Among all, the inside of moveable cursor, pointing to current result set, offset of each row.
  * int get_rows_count() const;
  * bool fetch_row(std::vector<MySQLCell>& row_arr);
  * bool fetch_row(std::map<std::string, MySQLCell>& row_map);
  * bool fetch_row(std::unordered_map<std::string, MySQLCell>& row_map);
  * bool fetch_row_nocopy(const void **data, size_t *len, int *data_type);
6. Take out all rows of the current result set directly: cursor->fetch_all() can be used to read all rows, the internal cursor used to record rows will be directly moved to the end; the cursor state will turn into MYSQL_STATUS_END:
  * bool fetch_all(std::vector<std::vector<MySQLCell>>& rows);
7. Return to the head of the current result set: If it is necessary to reread this result set, you can use cursor->rewind() to return to the head of the current result set, then read it following step 5 or step 6;
8. Get the next result set: as the data packet returned by the MySQL server may contain multiple result sets (for example, each select statement is a result set; or the multiple result set data returned by call procedure), user can use the cursor ->next_result_set() to skip to the next result set, the return value is false, representing all result sets have been fetched.
9. Return to the first result set: cursor->first_result_set() allows us to return to the head of all result sets, and then we can fetch the data again starting from step 3;
10. The specific data of each column MySQLCell: The row read in step 5 consists of multiple columns, and the result of each column is MySQLCell. The basic usage interfaces include:
  * int get_data_type(); return MYSQL_TYPE_LONG, MYSQL_TYPE_STRING... For details, please refer to [mysql_types.h](../src/protocol/mysql_types.h)
  * bool is_TYPE() const; TYPE is int, string, ulonglong, judge whether it is a certain type
  * TYPE as_TYPE() const; Same as above, read MySQLCell data in a certain type
  * void get_cell_nocopy(const void **data, size_t *len, int *data_type) const; nocopy interface
  
Entire example as follows:

~~~cpp
void task_callback(WFMySQLTask *task)
{
    // step-1. Judge state of task
    if (task->get_state() != WFT_STATE_SUCCESS)
    {
        fprintf(stderr, "task error = %d\n", task->get_error());
        return;
    }

    MySQLResultCursor cursor(task->get_resp());
    bool test_first_result_set_flag = false;
    bool test_rewind_flag = false;

begin:
    // step-2. Judge state of reply packet
    switch (resp->get_packet_type())
    {
    case MYSQL_PACKET_OK:
        fprintf(stderr, "OK. %llu rows affected. %d warnings. insert_id=%llu.\n",
                task->get_resp()->get_affected_rows(),
                task->get_resp()->get_warnings(),
                task->get_resp()->get_last_insert_id());
        break;

    case MYSQL_PACKET_EOF:
        do {
            fprintf(stderr, "cursor_status=%d field_count=%u rows_count=%u ",
                    cursor.get_cursor_status(), cursor.get_field_count(), cursor.get_rows_count());
            // step-3. Judge state of result set
            if (cursor.get_cursor_status() != MYSQL_STATUS_GET_RESULT)
                break;
            // step-4. Read each field. This is a nocopyapi
            const MySQLField *const *fields = cursor.fetch_fields();
            for (int i = 0; i < cursor.get_field_count(); i++)
            {
                fprintf(stderr, "db=%s table=%s name[%s] type[%s]\n",
                        fields[i]->get_db().c_str(), fields[i]->get_table().c_str(),
                        fields[i]->get_name().c_str(), datatype2str(fields[i]->get_data_type()));
            }

            // step-6. Read all rows, alternatively, while (cursor.fetch_row(map/vector)) fetch each row following step-5
            std::vector<std::vector<MySQLCell>> rows;

            cursor.fetch_all(rows);
            for (unsigned int j = 0; j < rows.size(); j++)
            {
                // step-10. Read each cell
                for (unsigned int i = 0; i < rows[j].size(); i++)
                {
                    fprintf(stderr, "[%s][%s]", fields[i]->get_name().c_str(),
                            datatype2str(rows[j][i].get_data_type()));
                    // step-10. Judge specific type is_string() and convert specific type as_string()
                    if (rows[j][i].is_string())
                    {
                        std::string res = rows[j][i].as_string();
                        fprintf(stderr, "[%s]\n", res.c_str());
                    } else if (rows[j][i].is_int()) {
                        fprintf(stderr, "[%d]\n", rows[j][i].as_int());
                    } // else if ...
                }
            }
        // step-8. Fetch next result set
        } while (cursor.next_result_set());

        if (test_first_result_set_flag == false)
        {
            test_first_result_set_flag = true;
            // step-9. Return to the first result set
            cursor.first_result_set();
            goto begin;
        }

        if (test_rewind_flag == false)
        {
            test_rewind_flag = true;
            // step-7. Return to the head of current result set
            cursor.rewind();
            goto begin;
        }
        break;

    default:
        fprintf(stderr, "Abnormal packet_type=%d\n", resp->get_packet_type());
        break;
    }
    return;
}
~~~

# WFMySQLConnection

As we are a highly concurrent asynchronous client, it means that we may have more than one connection to a server. The transaction and preparing of MySQL are both stateful. In order to ensure that a transaction or preparing has an exclusive connection, users can use our packaged secondary factory WFMySQLConnection to create tasks. We guarantee each WFMySQLConnection has an exclusive connection. For details, please refer to [WFMySQLConnection.h](../src/client/WFMySQLConnection.h).

### 1. Create and initialize WFMySQLConnection

When creating a WFMySQLConnection, you need to pass in an id, which must be globally unique, and subsequent calls will use this id to find the only corresponding connection.

Initialization requires passing in URL, and then the task created on this connection does not need to set the URL.

~~~cpp
class WFMySQLConnection
{
public:
    WFMySQLConnection(int id);
    int init(const std::string& url);
    ...
};
~~~

### 2. Create task and close connection

Task can be created by writing SQL request and callback function through create_query_task(), and the task must be sent from this connection.
Sometimes we need to manually close this connection. Because when we don’t use it any longer, the connection will remain till MySQL server timeout. During the period, if you use the same id and url to create a WFMySQLConnection, you can reuse this connection.
Therefore, if you are not going to reuse the connection, we recommend you to use create_disconnect_task() to create a task and manually close the connection.

~~~cpp
class WFMySQLConnection
{
public:
    ...
    WFMySQLTask *create_query_task(const std::string& query,
                                   mysql_callback_t callback);
    WFMySQLTask *create_disconnect_task(mysql_callback_t callback);
}
~~~

WFMySQLConnection is equivalent to a secondary factory. We stipulated that the life cycle of any factory object does not need to be maintained until the end of the task. The following code is completely legal:

~~~cpp
    WFMySQLConnection *conn = new WFMySQLConnection(1234);
    conn->init(url);
    auto *task = conn->create_query_task("SELECT * from table", my_callback);
    conn->deinit();
    delete conn;
    task->start();
~~~

### 3. Precautions

If BIGIN already starts but not yet COMMIT or ROLLBACK during the transaction, and the connection has been interrupted during the period, the connection will be automatically reconnected internally by the framework, and the user will get the ECONNRESET error in the next task request. At this time, the transaction statement not yet COMMIT already expires and needs to be reissued.

### 4. Prepare
User can also process PREPARE through WFMySQLConnection, which can be easily used as an anti-SQL injection. If reconnection occurs, you will also get an ECONNRESET error.

### 5. Complete example

~~~cpp
WFMySQLConnection conn(1);
conn.init("mysql://root@127.0.0.1/test");

// test transaction
const char *query = "BEGIN;";
WFMySQLTask *t1 = conn.create_query_task(query, task_callback);
query = "SELECT * FROM check_tiny FOR UPDATE;";
WFMySQLTask *t2 = conn.create_query_task(query, task_callback);
query = "INSERT INTO check_tiny VALUES (8);";
WFMySQLTask *t3 = conn.create_query_task(query, task_callback);
query = "COMMIT;";
WFMySQLTask *t4 = conn.create_query_task(query, task_callback);
WFMySQLTask *t5 = conn.create_disconnect_task(task_callback);
((*t1) > t2 > t3 > t4 > t5).start();
~~~
